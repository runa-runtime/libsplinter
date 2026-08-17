/**
 * Copyright 2025 Tim Post
 * License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
 *
 * @file splinter.c
 * @brief Main implementation of the libsplinter shared memory key-value store.
 *
 * libsplinter provides a high-performance, lock-free, shared-memory key-value
 * store and message bus. It is designed for efficient inter-process
 * communication (IPC), particularly for building process communities around 
 * local Large Language Model (LLM) runtimes.
 * 
 * You can use it like semantic "breadboard" - Have a good time!
 * https://splinterhq.github.io for docs
 * 
 * A note on variable naming choice: 
 * Single letter / globals are specific (H for header, S for the slot array, etc)
 * It's not being clever or prematurely optimal, it's saving room in the hot path
 * instruction cache for everything to fit.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include "splinter.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <poll.h>

#ifdef SPLINTER_NUMA_AFFINITY
#include <numa.h>
#include <numaif.h>
#endif // SPLINTER_NUMA_AFFINITY

/** @brief Base pointer to the memory-mapped region. */
static void *g_base = NULL;
/** @brief Total size of the memory-mapped region. */
static size_t g_total_sz = 0;
/** @brief Pointer to the header within the mapped region. */
static struct splinter_header *H;
/** @brief Pointer to the array of slots within the mapped region. */
static struct splinter_slot *S;
/** @brief Pointer to the start of the value storage area. */
static uint8_t *VALUES;
/** @brief Process-local eventfd used to signal epoch changes; -1 if not initialized. */
static int g_event_fd = -1;
/** @brief Physical slots covered by each event bus dirty-mask bit. Never 0. */
static size_t g_dirty_stripe = 1;

/* Forward declaration — defined near splinter_pulse_watchers */
static void splinter_event_bus_notify(size_t physical_idx);
/* Forward declaration — routing core shared with splinter_pulse_watchers() */
static void splinter_pulse_masks(uint64_t watcher_mask, uint64_t bloom);

/**
 * @brief Recomputes how many slots each dirty-mask bit covers.
 *
 * The mask is a fixed SPLINTER_EVENT_BUS_BITS wide, so a store larger than that
 * has to share bits. Slots are divided into contiguous stripes of this width.
 *
 * Slot count is fixed at creation (static geometry), so this is computed once
 * per mapping rather than on every notify — a 64-bit division is not worth
 * paying on the write hot path, which touches the bus on every set().
 *
 * ceil() here is load-bearing: it guarantees SPLINTER_EVENT_BUS_BITS * stripe
 * >= slots, so (slots-1)/stripe never exceeds the last bit of the mask.
 */
static void recompute_dirty_stripe(void) {
    uint32_t n = (H && H->slots) ? H->slots : 1;
    g_dirty_stripe = ((size_t)n + SPLINTER_EVENT_BUS_BITS - 1) / SPLINTER_EVENT_BUS_BITS;
    if (g_dirty_stripe == 0) g_dirty_stripe = 1;
}

/**
 * @brief Computes the 64-bit FNV-1a hash of a string.
 * @param s The null-terminated string to hash.
 * @return The 64-bit hash value.
 */
static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

/**
 * @brief Calculates the initial slot index for a given hash.
 * @param hash The hash of the key.
 * @param slots The total number of slots in the store.
 * @return The calculated slot index.
 */
static inline size_t slot_idx(uint64_t hash, uint32_t slots) {
    return (size_t)(hash % slots);
}

/**
 * @brief Adds a specified number of milliseconds to a timespec struct.
 * @param ts Pointer to the timespec struct to modify.
 * @param ms The number of milliseconds to add.
 */
static void add_ms(struct timespec *ts, uint64_t ms) {
    ts->tv_nsec += (ms % 1000) * NS_PER_MS;
    ts->tv_sec  += ms / 1000;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec  += 1;
    }
}

/**
 * @brief Internal helper to memory-map a file descriptor and set up global pointers.
 * @param fd The file descriptor to map.
 * @param size The size of the region to map.
 * @return 0 on success, -1 on failure.
 */
static int map_fd(int fd, size_t size) {
    g_total_sz = size;
    g_base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_base == MAP_FAILED) return -1;
    H = (struct splinter_header *)g_base;
    S = (struct splinter_slot *)(H + 1);
    VALUES = (uint8_t *)(S + H->slots);
    recompute_dirty_stripe();
    return 0;
}

/*
 * By default a new store (shm object or persistent file) is created with mode
 * 0666 masked by the shell's umask. On most GNU systems that leaves the store
 * readable by other local users, which can surprise anyone not steeped in UNIX
 * permissions. We deliberately do NOT change that default, because the common
 * case is local/automated use that may already lean on the inherited umask.
 *
 * As an opt-in, if SPLINTER_DEFAULT_UMASK is set in the environment we apply it
 * as the process umask for the duration of creation only, then restore the
 * previous one. The value is octal (e.g. "077" -> a private 0600 store).
 * Operators can take it from there with the usual tools (chmod, etc.).
 *
 * Returns the previous umask when an override was applied (pass it to
 * restore_env_umask), or (mode_t)-1 when nothing was changed. umask() only ever
 * returns a valid 0..0777 mask, so (mode_t)-1 is an unambiguous "untouched"
 * sentinel. Note: umask is process-global and not thread-safe, but store
 * creation is a setup-time operation, consistent with the rest of this library.
 */
static mode_t apply_env_umask(void) {
    const char *env = getenv("SPLINTER_DEFAULT_UMASK");
    if (!env || !*env) return (mode_t)-1;

    errno = 0;
    char *end = NULL;
    long val = strtol(env, &end, 8);
    if (errno != 0 || end == env || *end != '\0' || val < 0 || val > 0777)
        return (mode_t)-1;   /* unparseable / out of range: leave umask alone */

    return umask((mode_t)val);
}

static void restore_env_umask(mode_t prev) {
    if (prev != (mode_t)-1) umask(prev);
}

int splinter_create(const char *name_or_path, size_t slots, size_t max_value_sz) {
    int fd;

    if (slots <= 0 || max_value_sz <= 0) {
        errno = ENOTSUP;
        return -2;
    }

    mode_t prev_umask = apply_env_umask();
#ifdef SPLINTER_PERSISTENT
    /*
     * O_EXCL makes creation exclusive: if the store already exists, create fails
     * with EEXIST rather than adopting and reinitializing (potentially wiping)
     * it. Callers that want either-or use splinter_create_or_open(), which falls
     * back to open() on that failure. O_NOFOLLOW refuses to create through a
     * symlink planted at the path. This mirrors the shm_open() path below.
     */
    fd = open(name_or_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
#else
    fd = shm_open(name_or_path, O_RDWR | O_CREAT | O_EXCL, 0666);
#endif
    restore_env_umask(prev_umask);
    if (fd < 0) return -1;
    size_t region_sz = slots * max_value_sz;
    size_t total_sz  = sizeof(struct splinter_header) + slots * sizeof(struct splinter_slot) + region_sz;
    if (ftruncate(fd, (off_t)total_sz) != 0) return -1;
    if (map_fd(fd, total_sz) != 0) return -1;
    
    H->magic = SPLINTER_MAGIC;
    H->version = SPLINTER_VER;
    H->slots = (uint32_t)slots;
    H->max_val_sz = (uint32_t)max_value_sz;
    H->val_sz = total_sz;

    /*
     * map_fd() derived VALUES from H->slots, but on a fresh create the mapping
     * is zero-filled so H->slots read as 0 there, leaving VALUES aliased onto
     * the slot array. Now that the header is populated, recompute it so the
     * value arena starts after the slots. (splinter_open() is unaffected: it
     * maps a store whose header already carries the real slot count.)
     */
    VALUES = (uint8_t *)(S + H->slots);
    /* Same reason: map_fd() computed the stripe from a slot count of 0. */
    recompute_dirty_stripe();
    atomic_store_explicit(&H->val_brk, 0, memory_order_relaxed);
    atomic_store_explicit(&H->epoch, 1, memory_order_relaxed);
    /* New stores default to hybrid scrub (mop mode 1): see splinter_set_mop(). */
    atomic_store_explicit(&H->core_flags, SPL_SYS_AUTO_SCRUB | SPL_SYS_HYBRID_SCRUB, memory_order_relaxed);
    atomic_store_explicit(&H->user_flags, 0, memory_order_relaxed);
    atomic_store_explicit(&H->parse_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&H->last_failure_epoch, 0, memory_order_relaxed);

    // Initialize event bus
    for (size_t m = 0; m < SPLINTER_EVENT_BUS_MASK_WORDS; m++)
        atomic_store_explicit(&H->event_bus.dirty_mask[m], 0, memory_order_relaxed);
    atomic_store_explicit(&H->event_bus.owner_fd,  -1, memory_order_relaxed);
    atomic_store_explicit(&H->event_bus.owner_pid,  0, memory_order_relaxed);

    // Zero the cooperative-memory-scheduling bid table (Logic Shard election).
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        atomic_store_explicit(&H->shard_bids[b].shard_id,     0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b].pid,          0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b].intent,       SPL_INTENT_NONE, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b].priority,     0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b]._pad[0],      0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b]._pad[1],      0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b].duration_tsc, 0, memory_order_relaxed);
        atomic_store_explicit(&H->shard_bids[b].claimed_at,   0, memory_order_relaxed);
    }

    /*
     * bloom_watches is a single 64-entry routing table in the header, not
     * per-slot state. 0xFF marks a bloom bit as unwatched. This used to sit
     * inside the slot loop below, rewriting all 64 entries once per slot —
     * identical values every pass, and ~3.2M redundant atomic stores on a
     * 50k-slot store.
     */
    for (int b = 0; b < 64; b++) {
        atomic_store_explicit(&H->bloom_watches[b], 0xFF, memory_order_relaxed);
    }

    size_t i;
    for (i = 0; i < slots; ++i) {
        atomic_fetch_or(&S[i].type_flag, SPL_SLOT_DEFAULT_TYPE);
        atomic_store_explicit(&S[i].hash, 0, memory_order_relaxed);
        atomic_store_explicit(&S[i].epoch, 0, memory_order_relaxed);
        atomic_store_explicit(&S[i].ctime, 0, memory_order_relaxed);
        atomic_store_explicit(&S[i].atime, 0, memory_order_relaxed);
        atomic_store_explicit(&S[i].user_flag, 0, memory_order_relaxed);
        atomic_store_explicit(&S[i].watcher_mask, 0, memory_order_relaxed);
        S[i].val_off = (uint32_t)(i * max_value_sz);
        atomic_store_explicit(&S[i].val_len, 0, memory_order_relaxed);
        S[i].key[0] = '\0';
    }
    return 0;
}

int splinter_open(const char *name_or_path) {
    int fd;
#ifdef SPLINTER_PERSISTENT
    fd = open(name_or_path, O_RDWR);
#else
    fd = shm_open(name_or_path, O_RDWR, 0666);
#endif
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (map_fd(fd, (size_t)st.st_size) != 0) return -1;
    if (H->magic != SPLINTER_MAGIC || H->version != SPLINTER_VER) return -1;
    return 0;
}

#ifdef SPLINTER_NUMA_AFFINITY
void* splinter_open_numa(const char *name, int target_node) {
    if (numa_available() < 0) return NULL;
    int fd = shm_open(name, O_RDWR, 0666);
    struct stat st;
    fstat(fd, &st);
    void *addr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned long mask = (1UL << target_node);
    unsigned long maxnode = numa_max_node() + 1;
    if (mbind(addr, st.st_size, MPOL_BIND, &mask, maxnode, MPOL_MF_STRICT | MPOL_MF_MOVE) != 0) {
        perror("mbind failed");
    }
    return addr;
}
#endif //SPLINTER_NUMA_AFFINITY

int splinter_create_or_open(const char *name_or_path, size_t slots, size_t max_value_sz) {
    int ret = splinter_create(name_or_path, slots, max_value_sz);
    return (ret == 0 ? ret : splinter_open(name_or_path));
}

int splinter_open_or_create(const char *name_or_path, size_t slots, size_t max_value_sz) {
    int ret = splinter_open(name_or_path);
    return (ret == 0 ? ret : splinter_create(name_or_path, slots, max_value_sz));
}

int splinter_set_mop(unsigned int mode) {
    if (!H) return -2;
    switch (mode) {
        case 0:
            atomic_fetch_and(&H->core_flags, ~(SPL_SYS_AUTO_SCRUB | SPL_SYS_HYBRID_SCRUB));
            break;
        case 1:
            atomic_fetch_or(&H->core_flags, SPL_SYS_AUTO_SCRUB | SPL_SYS_HYBRID_SCRUB);
            break;
        case 2:
            splinter_config_set(H, SPL_SYS_AUTO_SCRUB);
            break;
        default:
            errno = EOPNOTSUPP;
            return -1;
    }
    return 0;
}

int splinter_get_mop(void) {
    if (!H) return -2;
    if (splinter_config_test(H, SPL_SYS_HYBRID_SCRUB)) return 1;
    if (splinter_config_test(H, SPL_SYS_AUTO_SCRUB)) return 2;
    return 0;
}

void splinter_purge(void) {
    if (!H || !S || !VALUES) return;
    for (uint32_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[i];
        uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_acquire);
        if (e & 1ull) continue;
        if (!atomic_compare_exchange_strong(&slot->epoch, &e, e + 1)) continue;
        uint32_t len = atomic_load_explicit(&slot->val_len, memory_order_relaxed);
        uint8_t *dst = VALUES + slot->val_off;
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == 0) {
            memset(dst, 0, H->max_val_sz);
        } else if (len < H->max_val_sz) {
            memset(dst + len, 0, H->max_val_sz - len);
        }
        atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
    }
}

void splinter_close(void) {
    if (g_event_fd >= 0) { close(g_event_fd); g_event_fd = -1; }
    if (g_base) munmap(g_base, g_total_sz);
    g_base = NULL; H = NULL; S = NULL; VALUES = NULL; g_total_sz = 0;
    g_dirty_stripe = 1;
}

int splinter_unset(const char *key) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    size_t i;
    for (i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        uint64_t slot_hash = atomic_load_explicit(&slot->hash, memory_order_acquire);
        if (slot_hash == h && strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t start_epoch = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (start_epoch & 1) { errno = EAGAIN; return -1; }
            int ret = (int)atomic_load_explicit(&slot->val_len, memory_order_acquire);
            /*
             * Capture the routing masks before the teardown below zeroes them.
             * Destroying a slot is exactly the event a watcher most needs to
             * hear about, but it is also the one event that erases the means of
             * delivering it, so the pulse has to be fired from a copy taken
             * while the slot still knows who was listening.
             */
            uint64_t dying_watchers = atomic_load_explicit(&slot->watcher_mask, memory_order_acquire);
            uint64_t dying_bloom    = atomic_load_explicit(&slot->bloom, memory_order_acquire);
            atomic_store_explicit(&slot->hash, 0, memory_order_release);
            if (splinter_config_test(H, SPL_SYS_AUTO_SCRUB)) {
                memset(VALUES + slot->val_off, 0, H->max_val_sz);
                memset(slot->key, 0, SPLINTER_KEY_MAX);
            } else {
                slot->key[0] = '\0';
            }
            atomic_store_explicit(&slot->type_flag, 0, memory_order_release);
            atomic_fetch_or(&slot->type_flag, SPL_SLOT_DEFAULT_TYPE);
            atomic_store_explicit(&slot->epoch, 0, memory_order_release);
            atomic_store_explicit(&slot->val_len, 0, memory_order_release);
#ifdef SPLINTER_EMBEDDINGS
            // This is necessary or overwrites may leave garbage at the end.
            memset(slot->embedding, 0, sizeof(float) * SPLINTER_EMBED_DIM);
#endif
            atomic_store_explicit(&slot->ctime, 0, memory_order_release);
            atomic_store_explicit(&slot->atime, 0, memory_order_release);
            atomic_store_explicit(&slot->user_flag, 0, memory_order_release);
            atomic_store_explicit(&slot->watcher_mask, 0, memory_order_release);
            atomic_store_explicit(&slot->bloom, 0, memory_order_release);
            atomic_fetch_add_explicit(&slot->epoch, 2, memory_order_release);
            /* Seqlock is closed (epoch even again); safe to signal. */
            splinter_pulse_masks(dying_watchers, dying_bloom);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            splinter_event_bus_notify((idx + i) % H->slots);
            return ret;
        }
    }
    return -1;
}

int splinter_set(const char *key, const void *val, size_t len) {
    if (!H || !key) return -2;
    if (len == 0) { errno = EINVAL; return -1; }
    if (len > H->max_val_sz) { errno = EMSGSIZE; return -1; }

    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    const size_t arena_sz = (size_t)H->slots * (size_t)H->max_val_sz;
    int contended = 0;

    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        uint64_t slot_hash = atomic_load_explicit(&slot->hash, memory_order_acquire);

        if (slot_hash == 0 || (slot_hash == h && strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0)) {
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            if (e & 1ull) { contended = 1; continue; }

            if (!atomic_compare_exchange_weak_explicit(&slot->epoch, &e, e + 1,
                                                      memory_order_acq_rel, memory_order_relaxed)) {
                contended = 1;
                continue;
            }

            if ((size_t)slot->val_off >= arena_sz || (size_t)slot->val_off + len > arena_sz) {
                /* val_off points outside the arena: the header geometry and the
                 * slot table disagree, which means the mapping is corrupt. */
                atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
                errno = EOVERFLOW;
                return -1;
            }

            uint8_t *dst = (uint8_t *)VALUES + slot->val_off;

            if (splinter_config_test(H, SPL_SYS_AUTO_SCRUB)) {
                if (splinter_config_test(H, SPL_SYS_HYBRID_SCRUB)) {
                    size_t scrub_len = (len + 63) & ~63;
                    if (scrub_len > H->max_val_sz) scrub_len = H->max_val_sz;
                    memset(dst, 0, scrub_len);
                } else {
                    memset(dst, 0, H->max_val_sz);
                }
            }
            
            memcpy(dst, val, len);
            atomic_store_explicit(&slot->val_len, (uint32_t)len, memory_order_release);

#ifdef SPLINTER_EMBEDDINGS
            // Don't let previous embeddings hang around between writes
            if (slot_hash == 0) {
                memset(slot->embedding, 0, sizeof(float) * SPLINTER_EMBED_DIM);
            }
#endif

            slot->key[0] = '\0';
            strncpy(slot->key, key, SPLINTER_KEY_MAX - 1);
            slot->key[SPLINTER_KEY_MAX - 1] = '\0';

            atomic_thread_fence(memory_order_release);
            atomic_store_explicit(&slot->hash, h, memory_order_release);
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            
            splinter_pulse_watchers(slot);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            splinter_event_bus_notify((idx + i) % H->slots);

            return 0;
        }
    }
    /*
     * Distinguish "no room" from "everyone was mid-write". The probe skips any
     * slot whose seqlock is held, so an exhausted loop does not on its own
     * prove the store is full — under contention a usable slot may simply have
     * been busy every time we looked at it. ENOSPC is permanent and should stop
     * a caller; EAGAIN is transient and should make it retry.
     */
    errno = contended ? EAGAIN : ENOSPC;
    return -1;
}

int splinter_get(const char *key, void *buf, size_t buf_sz, size_t *out_sz) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);

    size_t i;
    for (i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];

        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t start = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (start & 1) { errno = EAGAIN; return -1; }

            atomic_thread_fence(memory_order_acquire);

            size_t len = (size_t)atomic_load_explicit(&slot->val_len, memory_order_acquire);
            if (out_sz) *out_sz = len;

            if (buf) {
                if (buf_sz < len) { errno = EMSGSIZE; return -1; }
                memcpy(buf, VALUES + slot->val_off, len);
            }

            atomic_thread_fence(memory_order_acquire);
            uint64_t end = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (start == end && !(end & 1)) return 0;

            errno = EAGAIN;
            return -1;
        }
    }
    return -1;
}

int splinter_list(char **out_keys, size_t max_keys, size_t *out_count) {
    if (!H || !out_keys || !out_count) return -2;
    size_t count = 0, i;
    for (i = 0; i < H->slots && count < max_keys; ++i) {
        if (atomic_load_explicit(&S[i].hash, memory_order_acquire) &&
            atomic_load_explicit(&S[i].val_len, memory_order_acquire) > 0) {
            out_keys[count++] = S[i].key;
        }
    }
    *out_count = count;
    return 0;
}

int splinter_poll(const char *key, uint64_t timeout_ms) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    struct splinter_slot *slot = NULL;
    size_t i;
    for (i = 0; i < H->slots; ++i) {
        struct splinter_slot *s = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&s->hash, memory_order_acquire) == h &&
            strncmp(s->key, key, SPLINTER_KEY_MAX) == 0) {
            slot = s;
            break;
        }
    }
    if (!slot) return -1;

    uint64_t start_epoch = atomic_load_explicit(&slot->epoch, memory_order_acquire);
    if (start_epoch & 1) { errno = EAGAIN; return -1; }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    add_ms(&deadline, timeout_ms);

    struct timespec sleep_ts = {0, 10 * NS_PER_MS};
    while (1) {
        uint64_t cur_epoch = atomic_load_explicit(&slot->epoch, memory_order_acquire);
        if (cur_epoch & 1) { nanosleep(&sleep_ts, NULL); continue; }
        if (cur_epoch != start_epoch) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if ((now.tv_sec > deadline.tv_sec) ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            errno = ETIMEDOUT;
            return -1;
        }
        nanosleep(&sleep_ts, NULL);
    }
}

int splinter_get_header_snapshot(splinter_header_snapshot_t *snapshot) {
    if (!H) return -2;
    snapshot->magic = H->magic;
    snapshot->version = H->version;
    snapshot->slots = H->slots;
    snapshot->max_val_sz = H->max_val_sz;
    snapshot->core_flags = atomic_load_explicit(&H->core_flags, memory_order_acquire);
    snapshot->user_flags = atomic_load_explicit(&H->user_flags, memory_order_acquire);
    snapshot->epoch = atomic_load_explicit(&H->epoch, memory_order_acquire);
    snapshot->parse_failures = atomic_load_explicit(&H->parse_failures, memory_order_relaxed);
    snapshot->last_failure_epoch = atomic_load_explicit(&H->last_failure_epoch, memory_order_relaxed);
    return 0;
}

int splinter_get_slot_snapshot(const char *key, splinter_slot_snapshot_t *snapshot) {
    if (!H || !key || !snapshot) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots), i = 0;
    for (i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t start = 0, end = 0;
            do {
                start = atomic_load_explicit(&slot->epoch, memory_order_acquire);
                if (start & 1) continue;
                snapshot->hash = h;
                snapshot->epoch = start;
                snapshot->val_off = slot->val_off;
                snapshot->val_len = atomic_load_explicit(&slot->val_len, memory_order_relaxed);
                snapshot->type_flag = atomic_load_explicit(&slot->type_flag, memory_order_acquire);
                snapshot->user_flag = atomic_load_explicit(&slot->user_flag, memory_order_acquire);
                snapshot->ctime = atomic_load_explicit(&slot->ctime, memory_order_acquire);
                snapshot->atime = atomic_load_explicit(&slot->atime, memory_order_acquire);
                snapshot->bloom = atomic_load_explicit(&slot->bloom, memory_order_acquire);
                strncpy(snapshot->key, slot->key, SPLINTER_KEY_MAX);
#ifdef SPLINTER_EMBEDDINGS                
                memcpy(snapshot->embedding, slot->embedding, sizeof(float) * SPLINTER_EMBED_DIM);
#endif
                atomic_thread_fence(memory_order_acquire);
                end = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            } while (start != end);
            return 0;
        }
    }
    return -1;
}

#ifdef SPLINTER_EMBEDDINGS
int splinter_set_embedding(const char *key, const float *vec) {
    if (!H || !key || !vec) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            if (e & 1ull) return -1;
            uint64_t want = e + 1;
            if (!atomic_compare_exchange_strong(&slot->epoch, &e, want)) return -1;
            memcpy(slot->embedding, vec, sizeof(float) * SPLINTER_EMBED_DIM);
            atomic_thread_fence(memory_order_release);
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            /* Pulse after the seqlock closes, as splinter_set() does. */
            splinter_pulse_watchers(slot);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

int splinter_get_embedding(const char *key, float *embedding_out) {
    if (!H || !key || !embedding_out) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t start = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (start & 1) { errno = EAGAIN; return -1; }
            atomic_thread_fence(memory_order_acquire);
            memcpy(embedding_out, slot->embedding, sizeof(float) * SPLINTER_EMBED_DIM);
            uint64_t end = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (start == end) return 0;
            errno = EAGAIN;
            return -1;
        }
    }
    return -1;
}
#endif // SPLINTER_EMBEDDINGS

void splinter_config_set(struct splinter_header *hdr, uint8_t mask) {
    atomic_fetch_or(&hdr->core_flags, mask);
}
void splinter_config_clear(struct splinter_header *hdr, uint8_t mask) {
    atomic_fetch_and(&hdr->core_flags, ~mask);
}
int splinter_config_test(struct splinter_header *hdr, uint8_t mask) {
    return (atomic_load(&hdr->core_flags) & mask) != 0;
}
uint8_t splinter_config_snapshot(struct splinter_header *hdr) {
    return atomic_load(&hdr->core_flags);
}
void splinter_slot_usr_set(struct splinter_slot *slot, uint16_t mask) {
  atomic_fetch_or(&slot->user_flag, mask);
}
void splinter_slot_usr_clear(struct splinter_slot *slot, uint16_t mask) {
  atomic_fetch_and(&slot->user_flag, ~mask);
}
int splinter_slot_usr_test(struct splinter_slot *slot, uint16_t mask) {
  return (atomic_load(&slot->user_flag) & mask) != 0;
}
uint16_t splinter_slot_usr_snapshot(struct splinter_slot *slot) {
  return atomic_load(&slot->user_flag);
}

int splinter_set_named_type(const char *key, uint16_t mask) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            if (e & 1) { errno = EAGAIN; return -1; }
            if (!atomic_compare_exchange_strong(&slot->epoch, &e, e + 1)) {
                errno = EAGAIN; return -1;
            }
            atomic_thread_fence(memory_order_acquire);
            uint32_t current_len = atomic_load(&slot->val_len);
            if ((mask & SPL_SLOT_TYPE_BIGUINT) && current_len < 8) {
                uint32_t new_off = atomic_fetch_add(&H->val_brk, 8);
                if (new_off + 8 > H->val_sz) {
                    atomic_fetch_add(&slot->epoch, 1); 
                    errno = ENOMEM; return -1;
                }
                uint8_t *old_ptr = VALUES + slot->val_off;
                uint64_t converted_val = 0;
                if (current_len > 0 && old_ptr[0] >= '0' && old_ptr[0] <= '9') {
                    char tmp_buf[16] = {0};
                    memcpy(tmp_buf, old_ptr, (current_len < 15) ? current_len : 15);
                    converted_val = strtoull(tmp_buf, NULL, 0);
                } else {
                    memcpy(&converted_val, old_ptr, (current_len < 8) ? current_len : 8);
                }
                uint64_t *new_ptr = (uint64_t *)(VALUES + new_off);
                *new_ptr = converted_val;
                slot->val_off = new_off;
                atomic_store_explicit(&slot->val_len, 8, memory_order_relaxed);
            }
            atomic_store_explicit(&slot->type_flag, mask, memory_order_release);
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            /* Pulse after the seqlock closes, as splinter_set() does. */
            splinter_pulse_watchers(slot);
            atomic_fetch_add(&H->epoch, 1);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

int splinter_set_slot_time(const char *key, unsigned short mode, uint64_t epoch, size_t offset) {
  if (!H || !key) return -2;
  uint64_t h = fnv1a(key);
  size_t idx = slot_idx(h, H->slots), i;
  for (i = 0; i < H->slots; ++i) {
    struct splinter_slot *slot = &S[(idx + i) % H->slots];
    if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
      strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
        uint64_t start = atomic_load_explicit(&slot->epoch, memory_order_acquire);
        if (start & 1) { errno = EAGAIN; return -1; }
        atomic_thread_fence(memory_order_acquire);
        switch (mode) {
          case SPL_TIME_CTIME:
            atomic_store_explicit(&slot->ctime, epoch - offset, memory_order_release);
            return 0;
          case SPL_TIME_ATIME:
            atomic_store_explicit(&slot->atime, epoch - offset, memory_order_release);
            return 0;
          default:
            errno = ENOTSUP;
            return -2;
        }
    }
  }
  return -1;
}

int splinter_integer_op(const char *key, splinter_integer_op_t op, const void *mask) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    uint64_t m64 = 0;
    if (mask) memcpy(&m64, mask, sizeof(uint64_t));
    atomic_thread_fence(memory_order_acquire);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h && 
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint8_t type = atomic_load_explicit(&slot->type_flag, memory_order_relaxed);
            if (!(type & SPL_SLOT_TYPE_BIGUINT)) { errno = EPROTOTYPE; return -1; }
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            if (e & 1ull) { errno = EAGAIN; return -1; }
            if (!atomic_compare_exchange_strong_explicit(&slot->epoch, &e, e + 1,
                                                        memory_order_acquire, 
                                                        memory_order_relaxed)) {
                errno = EAGAIN; return -1;
            }
            uint64_t *val = (uint64_t *)(VALUES + slot->val_off);
            switch (op) {
                case SPL_OP_OR:  *val |= m64;  break;
                case SPL_OP_AND: *val &= m64;  break;
                case SPL_OP_XOR: *val ^= m64;  break;
                case SPL_OP_NOT: *val = ~(*val); break;
                case SPL_OP_INC: *val += m64;  break;
                case SPL_OP_DEC: *val -= m64;  break;
            }
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            /* Pulse after the seqlock closes, as splinter_set() does. */
            splinter_pulse_watchers(slot);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

const void *splinter_get_raw_ptr(const char *key, size_t *out_sz, uint64_t *out_epoch) {
    if (!H || !key) return NULL;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_acquire);
            if (out_epoch) *out_epoch = e;
            if (out_sz) *out_sz = (size_t)atomic_load_explicit(&slot->val_len, memory_order_relaxed);
            return (const void *)(VALUES + slot->val_off);
        }
    }
    return NULL;
}

uint64_t splinter_get_epoch(const char *key) {
    if (!H || !key) return 0;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            return atomic_load_explicit(&slot->epoch, memory_order_acquire);
        }
    }
    return 0;
}

int splinter_bump_slot(const char *key) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            if (e & 1ull) return -1; 
            uint64_t want = e + 1;
            if (!atomic_compare_exchange_strong(&slot->epoch, &e, want)) return -1;
            atomic_thread_fence(memory_order_release);
            splinter_pulse_watchers(slot);
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            return 0;
        }
    }
    return -1;
}

int splinter_retrain_slot(const char *key) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            /*
             * Force the seqlock into the writer-active (odd) state, scrub the
             * vectors, then republish at a known-good even epoch of 4. We store
             * the epoch outright rather than CAS so a slot left odd by a dead or
             * aborted trainer can still be reclaimed -- that is the whole point
             * of retrain. A concurrent reader sees the odd epoch during the
             * memset and retries; once it lands on 4 the slot is stable again.
             *
             * Epoch moving *backwards* is the documented signal that clients and
             * watchers must revalidate the key. This runs even without embeddings
             * compiled in, in which case it just resets the epoch and republishes.
             */
            atomic_store_explicit(&slot->epoch, 3, memory_order_release);
            atomic_thread_fence(memory_order_release);
#ifdef SPLINTER_EMBEDDINGS
            memset(slot->embedding, 0, sizeof(float) * SPLINTER_EMBED_DIM);
#endif
            atomic_thread_fence(memory_order_release);
            atomic_store_explicit(&slot->epoch, 4, memory_order_release);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            splinter_pulse_watchers(slot);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

int splinter_set_label(const char *key, uint64_t mask) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t after = atomic_fetch_or_explicit(&slot->bloom, mask,
                                                      memory_order_release) | mask;
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            /*
             * Route by the post-set bloom so a subscriber bound to the label we
             * just applied wakes on it. Without this the documented
             * WAITING -> SERVICING -> READY handshake never reaches anyone who
             * subscribed with splinter_watch_label_register(): pulse_masks() is
             * the only thing that maps bloom bits through H->bloom_watches[] to
             * a signal group, so a missing pulse means a missing transition.
             */
            splinter_pulse_masks(
                atomic_load_explicit(&slot->watcher_mask, memory_order_acquire), after);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

int splinter_unset_label(const char *key, uint64_t mask) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            uint64_t before = atomic_fetch_and_explicit(&slot->bloom, ~mask,
                                                        memory_order_release);
            atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
            /*
             * Route by the PRE-clear bloom. The whole point of clearing a label
             * is to tell whoever was watching it that the state moved on, and
             * that subscriber is bound to a bit the slot no longer carries — so
             * pulsing against the current bloom would wake everyone except the
             * one process that needed to hear it.
             */
            splinter_pulse_masks(
                atomic_load_explicit(&slot->watcher_mask, memory_order_acquire), before);
            splinter_event_bus_notify((idx + i) % H->slots);
            return 0;
        }
    }
    return -1;
}

int splinter_client_set_tandem(const char *base_key, const void **vals, 
                               const size_t *lens, uint8_t orders) {
    char tandem_name[SPLINTER_KEY_MAX];
    if (splinter_set(base_key, vals[0], lens[0]) != 0) return -1;
    for (uint8_t i = 1; i < orders; i++) {
        snprintf(tandem_name, sizeof(tandem_name), "%s%s%u", base_key, SPL_ORDER_ACCESSOR, i);
        if (splinter_set(tandem_name, vals[i], lens[i]) != 0) return -1;
    }
    return 0;
}

void splinter_client_unset_tandem(const char *base_key, uint8_t orders) {
    char tandem_name[SPLINTER_KEY_MAX];
    splinter_unset(base_key);
    for (uint8_t i = 1; i < orders; i++) {
        snprintf(tandem_name, sizeof(tandem_name), "%s%s%u", base_key, SPL_ORDER_ACCESSOR, i);
        splinter_unset(tandem_name);
    }
}

int splinter_watch_register(const char *key, uint8_t group_id) {
    if (!H || !key) return -2;
    if (group_id >= SPLINTER_MAX_GROUPS) { errno = EINVAL; return -2; }
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            atomic_fetch_or_explicit(&slot->watcher_mask, (1ULL << group_id), memory_order_release);
            return 0;
        }
    }
    return -1;
}

int splinter_watch_label_register(uint64_t bloom_mask, uint8_t group_id) {
    if (!H || group_id >= SPLINTER_MAX_GROUPS) return -2;
    for (int i = 0; i < 64; i++) {
        if (bloom_mask & (1ULL << i))
            atomic_store_explicit(&H->bloom_watches[i], group_id, memory_order_release);
    }
    return 0;
}

int splinter_pulse_keygroup(const char *key) {
    if (!H || !key) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);  
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            splinter_pulse_watchers(slot);
            return 0;
        }
    }
    return -1;
}

static void splinter_event_bus_notify(size_t physical_idx) {
    if (g_event_fd < 0 || !H) return;
    /*
     * Divide, don't fold. Modulo would scatter one bit across a residue class
     * spanning the whole store, so a watcher chasing bit k had to touch every
     * k-th slot. Dividing gives each bit a contiguous stripe instead, which a
     * watcher can rescan as a sequential walk. Stripe width is precomputed by
     * recompute_dirty_stripe(); it guarantees mapped stays inside the mask.
     */
    size_t mapped = physical_idx / g_dirty_stripe;
    atomic_fetch_or_explicit(
        &H->event_bus.dirty_mask[mapped / 64],
        (1ULL << (mapped % 64)),
        memory_order_release);
    /*
     * The fd is only a doorbell; the dirty mask above is the actual state, and
     * it is already published. So a dropped write costs nothing: EAGAIN means
     * the counter is saturated with a wakeup nobody has drained yet, and one
     * more increment would tell the reader nothing it isn't about to learn.
     * The eventfd is EFD_NONBLOCK precisely so this cannot block here — we are
     * potentially inside a seqlock write section, and stalling a writer here
     * would stall every reader queued behind it.
     */
    uint64_t u = 1;
    int wr = (int)write(g_event_fd, &u, sizeof(u));
    (void)wr;
}

/*
 * Routing core of splinter_pulse_watchers(), taking the two masks explicitly.
 *
 * Split out because the masks a pulse should route by are not always the ones
 * the slot currently carries. splinter_unset_label() has to wake the subscriber
 * bound to the label it just cleared, and splinter_unset() has to wake watchers
 * whose bits it is in the middle of erasing. Both need to pulse against a state
 * that no longer exists by the time the slot is consistent again.
 */
static void splinter_pulse_masks(uint64_t watcher_mask, uint64_t bloom) {
    if (!H) return;
    for (int i = 0; i < SPLINTER_MAX_GROUPS; i++) {
        if (watcher_mask & (1ULL << i))
            atomic_fetch_add_explicit(&H->signal_groups[i].counter, 1, memory_order_release);
    }
    for (int b = 0; b < 64; b++) {
        if (bloom & (1ULL << b)) {
            uint8_t g = atomic_load_explicit(&H->bloom_watches[b], memory_order_acquire);
            if (g < SPLINTER_MAX_GROUPS)
                atomic_fetch_add_explicit(&H->signal_groups[g].counter, 1, memory_order_release);
        }
    }
}

void splinter_pulse_watchers(struct splinter_slot *slot) {
    splinter_pulse_masks(
        atomic_load_explicit(&slot->watcher_mask, memory_order_acquire),
        atomic_load_explicit(&slot->bloom, memory_order_acquire));
}

int splinter_watch_unregister(const char *key, uint8_t group_id) {
    if (!H || !key || group_id >= SPLINTER_MAX_GROUPS) return -2;
    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
            atomic_fetch_and_explicit(&slot->watcher_mask, ~(1ULL << group_id), memory_order_release);
            return 0;
        }
    }
    return -1;
}

uint64_t splinter_get_signal_count(uint8_t group_id) {
    if (!H || group_id >= SPLINTER_MAX_GROUPS) return 0;
    return atomic_load_explicit(&H->signal_groups[group_id].counter, memory_order_acquire);
}

void splinter_enumerate_matches(uint64_t mask, 
    void (*callback)(const char *key, uint64_t epoch, void *data), void *user_data) 
{
    if (!H || !S) return;
    for (size_t i = 0; i < H->slots; i++) {
        struct splinter_slot *slot = &S[i];
        uint64_t h = atomic_load_explicit(&slot->hash, memory_order_acquire);
        if (h != 0 && (atomic_load_explicit(&slot->bloom, memory_order_acquire) & mask) == mask) {
            uint64_t ep = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
            callback(slot->key, ep, user_data);
        }
    }
}

int splinter_event_bus_init(void) {
    if (!H) return -1;
    /*
     * EFD_NONBLOCK is not optional here. Writers pulse this fd from inside
     * splinter_set() and friends; a blocking eventfd whose counter reached
     * UINT64_MAX-1 with no reader draining would wedge the writer mid-write.
     * See splinter_event_bus_notify() for why dropping the write is safe.
     */
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) return -1;
    atomic_store_explicit(&H->event_bus.owner_fd,  (int32_t)fd,        memory_order_release);
    atomic_store_explicit(&H->event_bus.owner_pid, (int32_t)getpid(), memory_order_release);
    g_event_fd = fd;
    return 0;
}

int splinter_event_bus_open(void) {
    if (!H) return -1;
    int32_t stored_fd  = atomic_load_explicit(&H->event_bus.owner_fd,  memory_order_acquire);
    int32_t stored_pid = atomic_load_explicit(&H->event_bus.owner_pid, memory_order_acquire);
    if (stored_fd < 0 || stored_pid <= 0) { errno = ENODEV; return -1; }

    /* Same process: dup() is the trivial path */
    if ((pid_t)stored_pid == getpid())
        return dup((int)stored_fd);

    /* Cross-process: use pidfd_getfd (Linux >= 5.6). */
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_getfd)
    int pidfd = (int)syscall(SYS_pidfd_open, (pid_t)stored_pid, 0);
    if (pidfd < 0) return -1;
    int fd = (int)syscall(SYS_pidfd_getfd, pidfd, (int)stored_fd, 0);
    close(pidfd);
    return fd;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int splinter_event_bus_wait(int fd, uint64_t timeout_ms) {
    if (fd < 0) return -1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int t = (timeout_ms == UINT64_MAX) ? -1 :
            (timeout_ms > (uint64_t)INT_MAX) ? INT_MAX : (int)timeout_ms;
    if (poll(&pfd, 1, t) <= 0) return -1;
    uint64_t val;
    return (read(fd, &val, sizeof(val)) == (ssize_t)sizeof(val)) ? 0 : -1;
}

void splinter_event_bus_close(int fd) {
    if (fd >= 0) close(fd);
}

void splinter_event_bus_get_dirty(uint64_t *out, size_t words) {
    if (!H || !out) return;
    size_t n = (words < SPLINTER_EVENT_BUS_MASK_WORDS) ? words : SPLINTER_EVENT_BUS_MASK_WORDS;
    for (size_t i = 0; i < n; i++)
        out[i] = atomic_load_explicit(&H->event_bus.dirty_mask[i], memory_order_acquire);
}

void splinter_event_bus_take_dirty(uint64_t *out, size_t words) {
    if (!H || !out) return;
    size_t n = (words < SPLINTER_EVENT_BUS_MASK_WORDS) ? words : SPLINTER_EVENT_BUS_MASK_WORDS;
    /*
     * Read-and-clear per word. acq_rel because we both publish the clear to
     * other writers and need the writes that set these bits to be visible to
     * whatever the caller does with the snapshot. Per-word rather than
     * whole-mask atomicity is fine: a bit set between two words is simply
     * reported on the next take, never lost.
     */
    for (size_t i = 0; i < n; i++)
        out[i] = atomic_fetch_and_explicit(&H->event_bus.dirty_mask[i], 0, memory_order_acq_rel);
}

size_t splinter_event_bus_stripe(void) {
    return g_dirty_stripe ? g_dirty_stripe : 1;
}

int splinter_set_as_system(const char *key) {
    if (!H || !S) return -2;
    size_t idx = 0;
    uint64_t h = fnv1a(key);
    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];
        if (atomic_load_explicit(&slot->hash, memory_order_acquire) == h &&
            strncmp(slot->key, key, SPLINTER_KEY_MAX) == 0) {
                atomic_store_explicit(&slot->type_flag, SPL_SLOT_TYPE_BINARY, memory_order_release);
                uint32_t system_sz = H->max_val_sz;
                atomic_store_explicit(&slot->val_len, system_sz, memory_order_release);
                return 0;
        }
    }
    return -1;
}

int splinter_append(const char *key, const void *data, size_t data_len, size_t *new_len) {
    if (!H || !key || !data) return -2;
    if (data_len == 0) return -2;

    uint64_t h = fnv1a(key);
    size_t idx = slot_idx(h, H->slots);

    for (size_t i = 0; i < H->slots; ++i) {
        struct splinter_slot *slot = &S[(idx + i) % H->slots];

        if (atomic_load_explicit(&slot->hash, memory_order_acquire) != h) continue;
        if (strncmp(slot->key, key, SPLINTER_KEY_MAX) != 0) continue;

        uint64_t e = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
        if (e & 1ull) { errno = EAGAIN; return -1; }

        if (!atomic_compare_exchange_weak_explicit(&slot->epoch, &e, e + 1,
                                                   memory_order_acq_rel,
                                                   memory_order_relaxed)) {
            errno = EAGAIN;
            return -1;
        }

        size_t cur_len = (size_t)atomic_load_explicit(&slot->val_len, memory_order_relaxed);
        if (cur_len + data_len > (size_t)H->max_val_sz) {
            atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
            errno = EMSGSIZE;
            return -1;
        }

        uint8_t *dst = VALUES + slot->val_off + cur_len;
        memcpy(dst, data, data_len);

        size_t total = cur_len + data_len;
        atomic_store_explicit(&slot->val_len, (uint32_t)total, memory_order_release);

        if (new_len) *new_len = total;

        atomic_fetch_add_explicit(&slot->epoch, 1, memory_order_release);
        splinter_pulse_watchers(slot);
        atomic_fetch_add_explicit(&H->epoch, 1, memory_order_relaxed);
        splinter_event_bus_notify((idx + i) % H->slots);

        return 0;
    }
    return -1;
}

/* 
 * Logic Shard Election & Voluntary Yield
 *
 * A fixed 32-slot bid table in the header coordinates cooperative memory
 * advisement. The election is a read-only O(32) scan deterministic from the
 * static bid data, so every shard independently computes the same sovereign
 * without a central arbiter. See splinter.h for the protocol contract.
 *
 * Memory ordering mirrors the slot-`hash` discipline: shard_id is the
 * publication point (acq_rel on CAS claim, release on clear, acquire on every
 * lookup/election read); descriptive fields are release on write, acquire on
 * read (hint-grade). Bid operations never bump H->epoch — they are scheduling
 * coordination metadata, not data writes, and must not perturb the watchers.
 */
/**
 * @brief Map an abstract splinter_intent_t to a POSIX_MADV_* advice constant.
 * Used by the daemons to translate a declared intent into the raw advice int
 * that splinter_madvise() forwards to posix_madvise().
 */
static int spl_intent_to_advice(uint8_t intent) __attribute__((unused));
static int spl_intent_to_advice(uint8_t intent) {
    switch (intent) {
        case SPL_INTENT_WILLNEED:   return POSIX_MADV_WILLNEED;
        case SPL_INTENT_SEQUENTIAL: return POSIX_MADV_SEQUENTIAL;
        case SPL_INTENT_RANDOM:     return POSIX_MADV_RANDOM;
        case SPL_INTENT_DONTNEED:   return POSIX_MADV_DONTNEED;
        case SPL_INTENT_NONE:
        default:                    return POSIX_MADV_NORMAL;
    }
}

/**
 * @brief True if the bid in slot b has outlived its declared window.
 * Unsigned subtraction; relies on splinter_now() monotonicity (the same
 * assumption the timing-backfill paths already make).
 */
static int spl_bid_expired(const struct splinter_shard_bid *bid, uint64_t now) {
    uint64_t claimed = atomic_load_explicit(&bid->claimed_at,   memory_order_acquire);
    uint64_t dur     = atomic_load_explicit(&bid->duration_tsc, memory_order_acquire);
    /* Half-open window: a bid is live while elapsed < dur and expires at dur.
     * Using >= (not >) makes duration_tsc == 0 mean "instantly expired" even
     * when the election reads the same tick the claim was stamped at, which a
     * strict > leaves as a one-tick race on coarse/emulated TSC clocks. */
    return (now - claimed) >= dur;
}

int splinter_shard_claim_ex(uint32_t shard_id, uint32_t pid, uint8_t intent,
                            uint8_t priority, uint64_t duration_tsc,
                            uint64_t claimed_at) {
    if (!H || shard_id == 0) return -2;

    /* First pass: refresh if we already own a slot (idempotent re-claim). */
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        if (atomic_load_explicit(&bid->shard_id, memory_order_acquire) == shard_id) {
            atomic_store_explicit(&bid->pid,          pid,          memory_order_release);
            atomic_store_explicit(&bid->intent,       intent,       memory_order_release);
            atomic_store_explicit(&bid->priority,     priority,     memory_order_release);
            atomic_store_explicit(&bid->duration_tsc, duration_tsc, memory_order_release);
            atomic_store_explicit(&bid->claimed_at,   claimed_at,   memory_order_release);
            return 0;
        }
    }

    /* Second pass: CAS-claim the first empty slot. */
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&bid->shard_id, &expected, shard_id,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            /* Slot is ours; publish the descriptive fields (release) after the
             * shard_id CAS. A racing election may see shard_id but stale fields
             * for one election; the next election corrects it (see header note). */
            atomic_store_explicit(&bid->pid,          pid,          memory_order_release);
            atomic_store_explicit(&bid->intent,       intent,       memory_order_release);
            atomic_store_explicit(&bid->priority,     priority,     memory_order_release);
            atomic_store_explicit(&bid->duration_tsc, duration_tsc, memory_order_release);
            atomic_store_explicit(&bid->claimed_at,   claimed_at,   memory_order_release);
            return 0;
        }
    }

    errno = ENOSPC;
    return -1;
}

int splinter_shard_claim(uint32_t shard_id, uint8_t intent,
                         uint8_t priority, uint64_t duration_tsc) {
    return splinter_shard_claim_ex(shard_id, (uint32_t)getpid(), intent,
                                   priority, duration_tsc, splinter_now());
}

int splinter_shard_rebid(uint32_t shard_id, uint8_t intent,
                         uint8_t priority, uint64_t duration_tsc) {
    if (!H || shard_id == 0) return -2;
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        if (atomic_load_explicit(&bid->shard_id, memory_order_acquire) == shard_id) {
            atomic_store_explicit(&bid->intent,       intent,       memory_order_release);
            atomic_store_explicit(&bid->priority,     priority,     memory_order_release);
            atomic_store_explicit(&bid->duration_tsc, duration_tsc, memory_order_release);
            atomic_store_explicit(&bid->claimed_at,   splinter_now(), memory_order_release);
            return 0;
        }
    }
    return -1;
}

int splinter_shard_release(uint32_t shard_id) {
    if (!H || shard_id == 0) return -2;
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        if (atomic_load_explicit(&bid->shard_id, memory_order_acquire) == shard_id) {
            /* Clear payload first, then release shard_id last so the slot only
             * becomes claimable after its descriptive fields are cleared. */
            atomic_store_explicit(&bid->pid,          0, memory_order_release);
            atomic_store_explicit(&bid->intent,       SPL_INTENT_NONE, memory_order_release);
            atomic_store_explicit(&bid->priority,     0, memory_order_release);
            atomic_store_explicit(&bid->duration_tsc, 0, memory_order_release);
            atomic_store_explicit(&bid->claimed_at,   0, memory_order_release);
            atomic_store_explicit(&bid->shard_id,     0, memory_order_release);
            return 0;
        }
    }
    return -1;
}

uint32_t splinter_shard_election(uint8_t *out_intent) {
    if (out_intent) *out_intent = SPL_INTENT_NONE;
    if (!H) return 0;

    uint64_t now = splinter_now();
    int live_protective = 0;   /* any unexpired WILLNEED/SEQUENTIAL present? */

    /* Pass 1: detect protective bids for the DONTNEED soft bumper. */
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        if (atomic_load_explicit(&bid->shard_id, memory_order_acquire) == 0) continue;
        if (spl_bid_expired(bid, now)) continue;
        uint8_t it = atomic_load_explicit(&bid->intent, memory_order_acquire);
        if (it == SPL_INTENT_WILLNEED || it == SPL_INTENT_SEQUENTIAL) {
            live_protective = 1;
            break;
        }
    }

    /* Pass 2: pick the winner. */
    int have_best = 0;
    uint32_t best_id = 0;
    uint8_t  best_intent = SPL_INTENT_NONE;
    uint8_t  best_prio = 0;
    uint64_t best_claimed = 0;
    uint32_t best_pid = 0;

    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        uint32_t id = atomic_load_explicit(&bid->shard_id, memory_order_acquire);
        if (id == 0) continue;
        if (spl_bid_expired(bid, now)) continue;

        uint8_t  it = atomic_load_explicit(&bid->intent,     memory_order_acquire);
        uint8_t  pr = atomic_load_explicit(&bid->priority,   memory_order_acquire);
        uint64_t ca = atomic_load_explicit(&bid->claimed_at, memory_order_acquire);
        uint32_t pd = atomic_load_explicit(&bid->pid,        memory_order_acquire);

        /* DONTNEED soft bumper: cannot win while live protective bids exist. */
        if (it == SPL_INTENT_DONTNEED && live_protective) continue;

        int beats;
        if (!have_best) {
            beats = 1;
        } else if (pr != best_prio) {
            beats = (pr > best_prio);          /* higher priority wins */
        } else if (ca != best_claimed) {
            beats = (ca < best_claimed);       /* earlier claim wins */
        } else {
            beats = (pd < best_pid);           /* lowest pid wins */
        }

        if (beats) {
            have_best = 1;
            best_id = id; best_intent = it; best_prio = pr;
            best_claimed = ca; best_pid = pd;
        }
    }

    if (out_intent) *out_intent = have_best ? best_intent : SPL_INTENT_NONE;
    return have_best ? best_id : 0;
}

int splinter_shard_is_sovereign(uint32_t shard_id) {
    if (!H) return -2;
    return (splinter_shard_election(NULL) == shard_id && shard_id != 0) ? 1 : 0;
}

int splinter_shard_table_snapshot(struct splinter_shard_bid_snapshot *out, size_t max) {
    if (!H || !out) return -2;

    uint64_t now = splinter_now();
    uint8_t  sov_intent = SPL_INTENT_NONE;
    uint32_t sovereign  = splinter_shard_election(&sov_intent);

    size_t n = (max < SPLINTER_MAX_SHARDS) ? max : SPLINTER_MAX_SHARDS;
    for (size_t b = 0; b < n; b++) {
        struct splinter_shard_bid *bid = &H->shard_bids[b];
        uint32_t id = atomic_load_explicit(&bid->shard_id, memory_order_acquire);
        out[b].shard_id     = id;
        out[b].pid          = atomic_load_explicit(&bid->pid,          memory_order_acquire);
        out[b].intent       = atomic_load_explicit(&bid->intent,       memory_order_acquire);
        out[b].priority     = atomic_load_explicit(&bid->priority,     memory_order_acquire);
        out[b].duration_tsc = atomic_load_explicit(&bid->duration_tsc, memory_order_acquire);
        out[b].claimed_at   = atomic_load_explicit(&bid->claimed_at,   memory_order_acquire);
        out[b].expired      = (id != 0) ? spl_bid_expired(bid, now) : 1;
        out[b].sovereign    = (id != 0 && id == sovereign) ? 1 : 0;
    }
    return (int)n;
}

int splinter_madvise(uint32_t shard_id, void *addr, size_t len,
                     int advice, uint64_t timeout_ticks) {
    /* Bound the re-election interval so a sovereign's expiry is noticed
     * promptly even if no eventfd wake fires. ~5 ms. */
    static const uint64_t EVENT_WAIT_CAP_MS = 5;

    if (!H) return -2;
    if (shard_id == 0) { errno = EINVAL; return -2; }

    /* The caller must hold a bid to participate (it need not be sovereign). */
    int present = 0;
    for (size_t b = 0; b < SPLINTER_MAX_SHARDS; b++) {
        if (atomic_load_explicit(&H->shard_bids[b].shard_id, memory_order_acquire) == shard_id) {
            present = 1;
            break;
        }
    }
    if (!present) { errno = EINVAL; return -2; }

    if (addr == NULL) {
        addr = VALUES;
        len  = (size_t)H->slots * (size_t)H->max_val_sz;
    }

    int has_deadline = (timeout_ticks != UINT64_MAX);
    uint64_t deadline = splinter_now() + timeout_ticks;  /* unused when !has_deadline */

    int bus_fd = splinter_event_bus_open();  /* -1 if not armed; poll-sleep fallback */

    /* posix_madvise()/madvise() require a page-aligned start address. Round the
     * start down to the enclosing page and extend the length to still cover the
     * requested range, so callers (and the NULL == whole-arena case) need not
     * worry about VALUES landing mid-page. */
    {
        long pg = sysconf(_SC_PAGESIZE);
        if (pg > 0) {
            uintptr_t a = (uintptr_t)addr;
            uintptr_t aligned = a & ~((uintptr_t)pg - 1);
            len += (size_t)(a - aligned);
            addr = (void *)aligned;
        }
    }

    for (;;) {
        if (splinter_shard_election(NULL) == shard_id) {
            int rc = posix_madvise(addr, len, advice);
            if (bus_fd >= 0) splinter_event_bus_close(bus_fd);
            if (rc == 0) return 0;
            errno = rc;   /* posix_madvise returns the error number directly */
            return -1;
        }

        if (timeout_ticks == 0) {            /* non-blocking defer */
            if (bus_fd >= 0) splinter_event_bus_close(bus_fd);
            errno = EAGAIN;
            return -1;
        }

        if (has_deadline && splinter_now() >= deadline) {
            if (bus_fd >= 0) splinter_event_bus_close(bus_fd);
            errno = ETIMEDOUT;
            return -1;
        }

        /* Block for a short capped interval so we re-elect on sovereign-window
         * expiry even without an explicit wake. The eventfd path still wakes
         * immediately on any bus write; the cap is just a safety net. */
        if (bus_fd >= 0) {
            splinter_event_bus_wait(bus_fd, EVENT_WAIT_CAP_MS);   /* ignore result; re-elect */
        } else {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)EVENT_WAIT_CAP_MS * NS_PER_MS };
            nanosleep(&ts, NULL);
        }
    }
}
