/**
 * Copyright 2025 Tim Post
 * License: Apache 2
 * @file splinter.h
 * @brief Public API for the libsplinter shared memory key-value store.
 *
 * This header defines the public functions for creating, opening, interacting
 * with, and closing a splinter store.
 * 
 * https://splinterhq.github.io for docs
 */

#ifndef SPLINTER_H
#define SPLINTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 
 * @brief Magic number to identify a splinter memory region. 
 * Spoiler: bytes 53 4C 4E 54 -> ASCII "S L N T" (never speaks unless spoken to)
 */
#define SPLINTER_MAGIC 0x534C4E54

/** @brief Version of the splinter data format (not the library version). */
#define SPLINTER_VER   4   /* was 3: header now carries the shard bid table */
/** @brief Maximum length of a key string, including null terminator. */
#define SPLINTER_KEY_MAX        64
/** @brief Nanoseconds per millisecond for time calculations. */
#define NS_PER_MS      1000000ULL
#ifdef SPLINTER_EMBEDDINGS
/** @brief The number of dimensions Splinter should support (OpenAI style is 768) */
#define SPLINTER_EMBED_DIM    768
#endif

/** @brief The maximum number of watch signal groups for a slot */
#define SPLINTER_MAX_GROUPS 64

/** @brief Maximum simultaneously-loaded Logic Shards. Equal to the seqlock
 *  writer ceiling — the bid table is never larger than the concurrency the
 *  epoch protocol already supports. */
#define SPLINTER_MAX_SHARDS 32

/** @brief Memory-intent classes for a shard bid (the `intent` field).
 *  These mirror the POSIX_MADV_* advice classes. SPL_INTENT_NONE marks a
 *  record whose owner has not yet declared (or has cleared) its intent.
 *  DONTNEED is accepted as a bid but is governed by the soft bumper in
 *  splinter_shard_election(): it cannot win while live WILLNEED/SEQUENTIAL
 *  bids exist. */
typedef enum {
    SPL_INTENT_NONE       = 0,
    SPL_INTENT_WILLNEED   = 1,
    SPL_INTENT_SEQUENTIAL = 2,
    SPL_INTENT_RANDOM     = 3,
    SPL_INTENT_DONTNEED   = 4
} splinter_intent_t;

/** @brief Width, in bits, of the event bus dirty mask.
 *
 *  This is NOT a cap on store size. A store may have any number of slots;
 *  nothing in splinter_create() checks against this value. Slots are mapped
 *  onto these bits in contiguous stripes — see struct splinter_event_bus. */
#define SPLINTER_EVENT_BUS_BITS 1024
/** @brief Number of 64-bit words in the event bus dirty mask */
#define SPLINTER_EVENT_BUS_MASK_WORDS (SPLINTER_EVENT_BUS_BITS / 64)

/** @deprecated Misleading name: this never was a cap on slots per store, only
 *  the width of the event bus dirty mask. Use SPLINTER_EVENT_BUS_BITS. Kept so
 *  existing sources and generated bindings keep compiling. */
#define SPLINTER_MAX_SLOTS SPLINTER_EVENT_BUS_BITS

/** @brief Reserved store system flags */
#define SPL_SYS_AUTO_SCRUB     (1u << 0)
#define SPL_SYS_HYBRID_SCRUB   (1u << 1)
#define SPL_SYS_RESERVED_2     (1u << 2)
#define SPL_SYS_RESERVED_3     (1u << 3)

/** @brief User store flags for aliasing */
#define SPL_SUSR1              (1u << 4)
#define SPL_SUSR2              (1u << 5)
#define SPL_SUSR3              (1u << 6)
#define SPL_SUSR4              (1u << 7)

/** @brief Named type flags */
#define SPL_SLOT_TYPE_VOID     (1u << 0)
#define SPL_SLOT_TYPE_BIGINT   (1u << 1)
#define SPL_SLOT_TYPE_BIGUINT  (1u << 2)
#define SPL_SLOT_TYPE_JSON     (1u << 3)
#define SPL_SLOT_TYPE_BINARY   (1u << 4)
#define SPL_SLOT_TYPE_IMGDATA  (1u << 5)
#define SPL_SLOT_TYPE_AUDIO    (1u << 6)
#define SPL_SLOT_TYPE_VARTEXT  (1u << 7)

/** @brief Default type for new slot writes */
#define SPL_SLOT_DEFAULT_TYPE SPL_SLOT_TYPE_VOID

/** @brief Per-slot user flags for aliasing */
#define SPL_FUSR1              (1u << 0)
#define SPL_FUSR2              (1u << 1)
#define SPL_FUSR3              (1u << 2)
#define SPL_FUSR4              (1u << 3)
#define SPL_FUSR5              (1u << 4)
#define SPL_FUSR6              (1u << 5)
#define SPL_FUSR7              (1u << 6)
#define SPL_FUSR8              (1u << 7)

/** @brief Modes for invoking slot timestamp updates */
#define SPL_TIME_CTIME         0
#define SPL_TIME_ATIME         1

/**
 * @brief The special character that accesses standard ordered sets in 
 * tandem keys. If you change it, change it to something that you're 
 * sure you won't see in your data. If keys might contain URLs, use
 * a very uncommon emoji.
 * 
 * If you set this to "." and your key is "car", then "car.1" would 
 * get you velocity of car, and so on. But if your keys look like:
 * 
 * 244cc1eb-baf8-41ea-beee-f634f3c00f61::yelp.com/rq/ray/z62h32
 * 
 * Then you have limited choices. 
 */
#define SPL_ORDER_ACCESSOR "."

/**
 * @brief Individual signal lane, aligned to prevent false sharing.
 */
struct splinter_signal_node {
    alignas(64) atomic_uint_least64_t counter;
};

/**
 * @brief Event bus for kernel-assisted epoch-change notifications via eventfd.
 *
 * The owner process calls splinter_event_bus_init() to create an eventfd and
 * record its pid + fd here.  Any process (including the owner) can then call
 * splinter_event_bus_open() to obtain a process-local fd to the same kernel
 * object via /proc/<owner_pid>/fd/<owner_fd>.
 *
 * dirty_mask tracks which slot indices changed since the last read, allowing
 * watchers to enumerate only modified slots instead of scanning the full store.
 *
 * The mask is a fixed SPLINTER_EVENT_BUS_BITS wide (128 bytes) regardless of
 * store size — it is meant to stay cache-resident. Slots are mapped onto it in
 * contiguous stripes of ceil(slots / SPLINTER_EVENT_BUS_BITS) slots each:
 *
 *     bit k covers physical slots [k*stripe, min((k+1)*stripe, slots))
 *
 * so bit k means "at least one slot in that contiguous range changed". For a
 * store of SPLINTER_EVENT_BUS_BITS slots or fewer the stripe is 1 and the
 * mapping is exact, one bit per slot. Call splinter_event_bus_stripe() to get
 * the current width rather than recomputing it. Because a stripe is contiguous,
 * rescanning it is a sequential walk; it does not scatter across the store.
 *
 * Bits are OR'd in by writers. splinter_event_bus_get_dirty() only peeks and
 * never clears, so a caller using it must diff against its own saved copy —
 * otherwise the mask saturates to all-ones over the life of the store and stops
 * discriminating. A single consumer should prefer splinter_event_bus_take_dirty(),
 * which snapshots and clears in one atomic step.
 */
struct splinter_event_bus {
    atomic_uint_least64_t dirty_mask[SPLINTER_EVENT_BUS_MASK_WORDS];
    atomic_int_least32_t  owner_fd;
    atomic_int_least32_t  owner_pid;
};

/**
 * @brief One cooperative-memory-scheduling bid. 32 of these live in the
 * header. Packed to ~32 bytes so all 32 fit in ~1 KB (intentionally NOT
 * individually cache-line aligned: claims/releases are rare and elections
 * are read-only, so false sharing on this table is a non-issue, and the
 * thesis budgets ~1 KB, not 2 KB).
 */
struct splinter_shard_bid {
    atomic_uint_least32_t shard_id;     /**< 0 = empty slot. Claimed via CAS. */
    atomic_uint_least32_t pid;          /**< getpid() of the claimant; PID tie-break. */
    atomic_uint_least8_t  intent;       /**< splinter_intent_t. */
    atomic_uint_least8_t  priority;     /**< 0-255, higher wins the election. */
    atomic_uint_least8_t  _pad[2];      /**< explicit padding; keep layout stable. */
    atomic_uint_least64_t duration_tsc; /**< declared window in splinter_now() ticks. */
    atomic_uint_least64_t claimed_at;   /**< splinter_now() at claim / last re-bid. */
};

/**
 * @struct splinter_header
 * @brief Defines the header structure for the shared memory region.
 *
 * This header contains metadata for the entire splinter store, including
 * magic number for validation, version, and overall store configuration.
 *
 * NOTE: We add parse_failures/last_failure_epoch for diagnostics.
 */
struct splinter_header {
    /** @brief Magic number (SPLINTER_MAGIC) to verify integrity. */
    uint32_t magic;
    /** @brief Data layout version (SPLINTER_VER). */
    uint32_t version;
    /** @brief Total number of available key-value slots. */
    uint32_t slots;
    /** @brief Maximum size for any single value. */
    uint32_t max_val_sz;
    /** @brief Global epoch, incremented on any write. Used for change detection. */
    atomic_uint_least64_t epoch;
    /** @brief Core feature flags  */
    atomic_uint_least8_t core_flags;
    /** @brief User-defined feature flags */
    atomic_uint_least8_t user_flags;
    /** @brief Track the next-available value region */
    atomic_uint_least32_t val_brk;
    /** @brief Running total size of the arena */
    uint32_t val_sz;
    /** @brief Memory alignment (e.g  64) */
    uint32_t alignment;

    /* Diagnostics: counts of parse failures reported by clients / harnesses */
    atomic_uint_least64_t parse_failures;
    atomic_uint_least64_t last_failure_epoch;

    // Maps each of the 64 bloom bits to a specific Signal Group (0-63)
    // 0xFF indicates no watch for that bit.
    atomic_uint_least8_t bloom_watches[64];

    // The Signal Arena for epoll-backed notifications
    alignas(64) struct splinter_signal_node signal_groups[SPLINTER_MAX_GROUPS];

    // Event bus for kernel-assisted wake-up on epoch change
    alignas(64) struct splinter_event_bus event_bus;

    // Cooperative memory-scheduling bid table (Logic Shard election).
    // Placed last so prior field offsets are undisturbed. The whole array is
    // 64-byte aligned (one fresh cache line) but records inside are packed.
    alignas(64) struct splinter_shard_bid shard_bids[SPLINTER_MAX_SHARDS];
};


/**
 * @struct splinter_slot
 * @brief Defines a single key-value slot in the hash table.
 *
 * Each slot holds a key, its value's location and length, and metadata
 * for concurrent access and change tracking.
 *
 * We changed val_len to atomic to avoid tearing on platforms where a plain
 * 32-bit write could be observed partially by a reader.
 */
struct splinter_slot {
    /** @brief The FNV-1a hash of the key. 0 indicates an empty slot. */
    alignas(64) atomic_uint_least64_t hash;
    /** @brief Per-slot epoch, incremented on write to this slot. Used for polling. */
    atomic_uint_least64_t epoch;
    /** @brief Offset into the VALUES region where the value data is stored. */
    uint32_t val_off;
    /** @brief The actual length of the stored value data (atomic). */
    atomic_uint_least32_t val_len;
    /** @brief The type-naming flags for slot typing */
    atomic_uint_least8_t type_flag;
    /** @brief The user-defined flags for slot features */
    atomic_uint_least8_t user_flag;
    /** @brief Watcher signal group for multi-watching */
    atomic_uint_least64_t watcher_mask;
    /** @brief The time a slot was created (optional; must be set by the client) */
    atomic_uint_least64_t ctime;
    /** @brief The last time the slot was meaningfully accessed (optional; must be set by the client) */
    atomic_uint_least64_t atime;
    /** @brief The 64-bit Bloom filter / Label mask */
    atomic_uint_least64_t bloom;
    /** @brief The null-terminated key string. */
    char key[SPLINTER_KEY_MAX];
#ifdef SPLINTER_EMBEDDINGS
    float embedding[SPLINTER_EMBED_DIM];
#endif
};

/**
 * @struct splinter_header_snapshot
 * @brief structure to hold splinter bus snapshots
 */
typedef struct splinter_header_snapshot {
    /** @brief Magic number (SPLINTER_MAGIC) to verify integrity. */
    uint32_t magic;
    /** @brief Data layout version (SPLINTER_VER). */
    uint32_t version;
    /** @brief Total number of available key-value slots. */
    uint32_t slots;
    /** @brief Maximum size for any single value. */
    uint32_t max_val_sz;
    /** @brief Global epoch, incremented on any write. Used for change detection. */
    uint64_t epoch;
    /** @Brief holds the slot type flags */
    uint8_t core_flags;
    /** @Brief holds the slot user flags */
    uint8_t user_flags;

    /* Diagnostics: counts of parse failures reported by clients / harnesses */
    uint64_t parse_failures;
    uint64_t last_failure_epoch;
} splinter_header_snapshot_t;

/**
 * @brief Copy the current atomic Splinter header structure into a corresponding
 * non-atomic client version.
 * @param snapshot A splinter_header_snaphshot_t structure to receive the values.
 * @return -1 on failure, 0 on success.
 */
int splinter_get_header_snapshot(splinter_header_snapshot_t *snapshot);

/**
 * @structure splinter_slot_snapshot
 * @brief A structure to hold a snapshot of a single slot
 */
typedef struct splinter_slot_snapshot {
    /** @brief The FNV-1a hash of the key. 0 indicates an empty slot. */
    uint64_t hash;
    /** @brief Per-slot epoch, incremented on write to this slot. Used for polling. */
    uint64_t epoch;
    /** @brief Offset into the VALUES region where the value data is stored. */
    uint32_t val_off;
    /** @brief The actual length of the stored value data (atomic). */
    uint32_t val_len;
    /** @brief The slot type flags */
    uint8_t type_flag;
    /** @brief The slot user flags */
    uint8_t user_flag;
    /** @brief Storage for creation time */
    uint64_t ctime;
    /** @brief Storage for access time */
    uint64_t atime;
    /** @brief Bloom bits */
    uint64_t bloom;
    /** @brief The null-terminated key string. */
    char key[SPLINTER_KEY_MAX];
#ifdef SPLINTER_EMBEDDINGS
    float embedding[SPLINTER_EMBED_DIM];
#endif
} splinter_slot_snapshot_t;

/**
 * @struct splinter_shard_bid_snapshot
 * @brief Non-atomic mirror of a single bid slot for inspection/audit.
 */
struct splinter_shard_bid_snapshot {
    uint32_t shard_id;
    uint32_t pid;
    uint8_t  intent;
    uint8_t  priority;
    uint64_t duration_tsc;
    uint64_t claimed_at;
    int      expired;   /**< computed at snapshot time vs splinter_now() */
    int      sovereign; /**< 1 if this record won the election at snapshot time */
};

/**
 * @brief for atomic integer operations
 */
typedef enum {
    SPL_OP_AND,
    SPL_OP_OR,
    SPL_OP_XOR,
    SPL_OP_NOT,
    SPL_OP_INC,
    SPL_OP_DEC
} splinter_integer_op_t;

/**
 * SPLINTER AI PRIMER — READ THIS BEFORE ANY OTHER FUNCTION
 * =========================================================
 * Audience: LLM agents, code-generation models, agentic frameworks.
 * Purpose:  Establish the invariants, risk topology, and operational
 *           geometry of Splinter before any API call is attempted.
 *
 * WHAT SPLINTER IS
 * ----------------
 * Splinter is a passive shared-memory substrate. It is not a database,
 * not a server, and not a message broker. It is a memory-mapped manifold
 * where multiple processes share the same physical RAM directly.
 * There is no central arbiter. There is no socket. There is no copy.
 * Reads and writes happen at L3 cache speeds (~10 cycles/op on modern
 * hardware). This is not an abstraction — it is the hardware bus.
 *
 * If you are used to Redis, SQLite, or any client-server store, reset
 * your assumptions. The "socket tax" (kernel interrupts, context switches,
 * serialization) does not exist here. Neither does its protection.
 * You are operating closer to the metal than usual. Respect that.
 *
 * THE ONE INVARIANT YOU MUST NEVER VIOLATE
 * -----------------------------------------
 * Every slot has a 64-bit epoch counter.
 * EVEN epoch = slot is stable, safe to read or write.
 * ODD epoch  = writer is active, slot is in transition.
 *
 * Before ANY operation on a slot: check the epoch.
 * After a read: check it again. If it changed, the read is torn — retry.
 * This is the seqlock protocol. It is not optional. It is not defensive
 * programming. It is the only thing standing between you and silent
 * data corruption across process boundaries.
 *
 * Pattern (always):
 *   uint64_t e1 = splinter_get_epoch(key);
 *   if (e1 & 1) { errno = EAGAIN; retry or abort; }
 *   // ... read data ...
 *   uint64_t e2 = splinter_get_epoch(key);
 *   if (e1 != e2) { errno = EAGAIN; retry or abort; }
 *   // data is consistent
 *
 * ONE WRINKLE — THE EPOCH CAN MOVE BACKWARD
 * ------------------------------------------
 * Epochs normally only advance. splinter_retrain_slot() deliberately drives a
 * slot's epoch *backward* to a known-good even value (4), scrubbing its vector
 * and republishing. This is the documented "revalidate me" signal for trainers,
 * and it is the only sanctioned way to free a seqlock left stuck odd by a
 * crashed writer. Your seqlock check (e1 != e2) catches it correctly — any
 * change, forward OR backward, means retry/revalidate. Never assume the epoch
 * is monotonic, and never cache a value across a backward jump.
 *
 * EAGAIN IS NOT AN ERROR — IT IS A SIGNAL
 * ----------------------------------------
 * When any function returns -1 with errno == EAGAIN, the slot is
 * momentarily contested. The correct response is always retry with
 * backoff. The state is consistent — nothing is broken. A writer
 * was active. Wait and try again.
 *
 * When a function returns -2, the caller made an error (NULL key,
 * NULL store, invalid argument). Do not retry. Fix the call.
 *
 * Other errno values you will meet are NOT contention and must not be retried
 * verbatim: EMSGSIZE (a value or append exceeds max_val_sz — a geometry
 * problem), ENOSPC (the store is full, or the 32-slot shard bid table is full),
 * EOVERFLOW (a slot's value offset falls outside the arena — the mapping is
 * corrupt, stop), EINVAL (a zero-length value), and ETIMEDOUT (a bounded
 * cooperative-madvise wait expired). Only EAGAIN means "the same call will
 * succeed once the writer leaves."
 *
 * Note that splinter_set() distinguishes a genuinely full store from transient
 * contention. Its probe skips slots whose seqlock is held, so exhausting the
 * probe does not by itself prove there is no room: if any candidate was busy
 * you get EAGAIN (retry), and only if none were do you get ENOSPC (give up).
 *
 * RISK TOPOLOGY — KNOW BEFORE YOU CALL
 * --------------------------------------
 * DESTRUCTIVE (epoch reset/rewind, data or vectors zeroed, watchers pulsed):
 *   splinter_unset()         — frees the slot, zeroes key+value, clears labels
 *   splinter_retrain_slot()  — scrubs the embedding, drives the epoch *backward*
 *   splinter_purge()         — sweeps stale bytes past val_len across every slot.
 *                              Does NOT pulse watchers and does not advance any
 *                              epoch: it touches every slot in the store, so
 *                              signalling per slot would be a wake storm, and
 *                              it only scrubs bytes already past val_len — no
 *                              live value changes. Nothing to tell a watcher.
 *
 * HIGH (permanent label state change, signal propagation, real syscalls):
 *   splinter_set_label(), splinter_unset_label(),
 *   splinter_watch_label_register(), splinter_bump_slot(),
 *   splinter_pulse_keygroup(), splinter_set_as_system(),
 *   splinter_madvise()       — issues a real posix_madvise() if you win the election
 *
 * MEDIUM (value overwrite, epoch advance, watchers pulsed):
 *   splinter_set(), splinter_append(), splinter_set_embedding(),
 *   splinter_integer_op(), splinter_set_named_type(),
 *   splinter_client_set_tandem(),
 *   splinter_event_bus_take_dirty() — clears shared notification state, so a
 *                                     second watcher can miss the bits you took
 *
 * LOW-WRITE (metadata only — no value change, no epoch advance, no pulse):
 *   splinter_set_slot_time() — overwrites a slot's ctime/atime and nothing
 *                              else. Watchers are not signalled: a timestamp
 *                              is not a state transition anyone waits on.
 *
 * CONFIG (mutate store or shard policy, not slot data — own them deliberately):
 *   splinter_set_mop(), splinter_event_bus_init(),
 *   splinter_shard_claim(), splinter_shard_claim_ex(),
 *   splinter_shard_rebid(), splinter_shard_release()
 *
 * LOW (read-only or self-scoped, no cross-process data loss, safe to retry):
 *   splinter_get(), splinter_get_epoch(), splinter_get_embedding(),
 *   splinter_get_raw_ptr(), splinter_list(), splinter_poll(),
 *   splinter_get_signal_count(), splinter_get_header_snapshot(),
 *   splinter_get_slot_snapshot(), splinter_get_mop(),
 *   splinter_shard_election(), splinter_shard_is_sovereign(),
 *   splinter_shard_table_snapshot(),
 *   splinter_event_bus_open(), splinter_event_bus_wait(),
 *   splinter_event_bus_get_dirty(), splinter_event_bus_stripe()
 *
 * If your confidence in the correctness of your inputs is below ~0.90,
 * do not call DESTRUCTIVE or HIGH risk functions. Retrieve a slot
 * snapshot first, verify, then act.
 *
 * splinter_get_raw_ptr() deserves special mention: it returns a pointer
 * directly into shared memory. That pointer is live. Another process can
 * change or zero the memory at that address between the time you receive
 * the pointer and the time you dereference it. Always pair with epoch
 * verification. Never hold the pointer across a yield or sleep.
 *
 * SIGNAL FLOW — HOW PROCESSES COMMUNICATE
 * -----------------------------------------
 * Splinter has 64 signal groups (0-63), each an atomic counter.
 * Writers do not notify readers directly. Instead:
 *   write → splinter_pulse_watchers() → signal_group counter increments
 *
 * Readers poll their signal group counter. When it changes, they scan
 * for modified epochs. This is the entire pub/sub system.
 * splinter_get_signal_count(group_id) is your heartbeat check.
 * splinter_watch_register(key, group_id) subscribes a key to a group.
 * splinter_watch_label_register(bloom_mask, group_id) subscribes by label.
 *
 * Polling a counter is the floor. The kernel-assisted path is the EVENT BUS:
 * the owner process calls splinter_event_bus_init() once to arm an eventfd; any
 * process then calls splinter_event_bus_open() and blocks in
 * splinter_event_bus_wait(fd, timeout_ms) until a write advances the global
 * epoch. On wake, splinter_event_bus_get_dirty() hands you a bitmask of the
 * slot indices that changed, so you scan only what moved instead of sweeping
 * the whole store. Prefer this to a busy spin whenever you can afford to block.
 *
 * BLOOM LABELS — SEMANTIC ROUTING, NOT SEARCH
 * ---------------------------------------------
 * Each slot has a 64-bit bloom mask. Labels are OR'd in atomically.
 * splinter_enumerate_matches(mask, callback, data) visits only slots
 * where (slot->bloom & mask) == mask. This is O(slots), not O(1).
 * Use it for batch operations, not hot-path queries.
 *
 * Labels are persistent until explicitly cleared with splinter_unset_label().
 * splinter_unset() clears them as part of slot destruction.
 *
 * A standard label lifecycle for inference coordination:
 *   Client sets WAITING label → sidecar detects via enumerate_matches
 *   Sidecar clears WAITING, sets SERVICING → does work, appends tokens
 *   Sidecar clears SERVICING, sets READY → consumer reads result
 * Never skip a label transition. Governance observes the bloom directly.
 *
 * GEOMETRY AND MEMORY — WHAT YOU CANNOT CHANGE AT RUNTIME
 * ---------------------------------------------------------
 * Splinter has static geometry. Slot count and max value size are fixed
 * at creation (splinter_create()). You cannot resize a live store.
 * If you fill the store, splinter_set() returns -1. There is no eviction.
 * Plan your keyspace before you write your first key.
 *
 * max_val_sz is a hard ceiling per slot. splinter_append() will return
 * -1 with errno == EMSGSIZE if an append would overflow. Check before
 * appending in a loop. splinter_get_header_snapshot() gives you the
 * geometry without risk.
 *
 * The store is a flat arena of (slots × max_val_sz) bytes plus a header.
 * 64-byte alignment is mandatory for slot structures. If you modify slot
 * geometry in a fork, verify alignment with the provided test before use.
 *
 * STALE BYTES BEYOND val_len — THE MOP
 * --------------------------------------
 * A slot's value region is max_val_sz wide but only val_len bytes are live.
 * splinter_get() always honors val_len, but splinter_get_raw_ptr() does not —
 * the bytes past out_sz may be leftovers from a previous, longer write to the
 * same slot. The store's "mop" mode governs scrubbing: 0 = off, 1 = hybrid
 * (default; clears the new length plus a 64-byte-aligned slop region so SIMD
 * loads can't see stale data), 2 = full boil (zeroes the whole slot, costlier).
 * Query it with splinter_get_mop(). If you read raw and contamination matters
 * (LLM memory, forensics, verifiable research) do not trust bytes beyond out_sz,
 * and consider splinter_purge() during a quiescent maintenance window.
 *
 * COOPERATIVE MEMORY ADVISEMENT — DO NOT madvise() BEHIND THE BUS
 * ----------------------------------------------------------------
 * As of format v4 (SPLINTER_VER 4) the header carries a 32-slot Logic Shard bid
 * table. When multiple processes share one store, independent posix_madvise()
 * calls on the same arena make the kernel page-cache thrash and destroy L3
 * residency. The cure is coordination, not a lock: declare intent with
 * splinter_shard_claim() (WILLNEED / SEQUENTIAL / RANDOM / DONTNEED, a 0-255
 * priority, and a duration in splinter_now() ticks), then issue advice through
 * splinter_madvise() — never raw posix_madvise() on a shared store.
 *
 * The election (splinter_shard_election) is a read-only O(32) scan: every
 * process computes the same sovereign from static bid data, with no arbiter.
 * Only the sovereign's advice lands; non-sovereigns defer (EAGAIN), block, or
 * time out depending on the timeout you pass. DONTNEED is a soft-bumped
 * second-class citizen — it cannot win while any live WILLNEED/SEQUENTIAL bid
 * exists, so treat it as maintenance-window-only. Bids never touch H->epoch, so
 * claiming or releasing a bid does NOT perturb watchers. Latency-sensitive
 * callers: short window, high priority, re-bid often, advise non-blocking.
 * Backfills/sweeps: SEQUENTIAL, low priority, non-blocking, and accept deferral
 * as the correct outcome rather than fighting a live completer.
 *
 * WHAT SPLINTER IS NOT DESIGNED FOR
 * -----------------------------------
 * - Multi-machine replication (use a real database for that)
 * - Durable ACID transactions (seqlock is crash-consistent, not ACID)
 * - Dynamic schema evolution at runtime (geometry is fixed)
 * - High-cardinality keyspaces without pre-planned slot counts
 * - Any operation where you cannot tolerate EAGAIN and retry
 *
 * MECHANICAL SYMPATHY NOTE
 * -------------------------
 * Splinter is designed to live in the L3 cache. On Tiger Lake i3 hardware
 * this yields ~3.2M ops/sec at ~937 cycles/op. On Zen 4/5 with the
 * manifold pinned to L3, projected CPO drops to single digits.
 * The instruction path is intentionally minimal so the hot path fits
 * in the CPU instruction cache. Do not add complexity to the call path
 * without measuring the cache impact. Every instruction you add is
 * potentially an eviction.
 *
 * FOR AGENTIC / AUTONOMOUS USE
 * -----------------------------
 * If you are an agent operating autonomously on a Splinter store:
 *
 * 1. Always retrieve a header snapshot before your first write to
 *    verify you understand the geometry of this specific store instance.
 *
 * 2. Never call splinter_unset() on a key you did not create unless
 *    you have verified via bloom label and epoch that no other process
 *    is mid-transaction on that slot.
 *
 * 3. splinter_bump_slot() without a write will pulse watchers and
 *    advance the epoch. Other processes will interpret this as a state
 *    change. Only call it if you intend to signal.
 *
 * 4. The store has no authentication, no permissions, and no namespacing
 *    beyond what you impose with key naming conventions. You are trusted
 *    by the system the moment you have the bus name. Act accordingly.
 *
 * 5. If you are in an agentic loop and encounter repeated EAGAIN on the
 *    same key across multiple cycles, stop and surface this to a supervisor.
 *    You may be in contention with another agent, or a writer may have
 *    crashed with an odd epoch. This requires human or supervisory judgment,
 *    not more retries.
 *
 * 6. High-stakes or irreversible operations (unset, type promotion,
 *    label registration against signal groups) should be preceded by
 *    a full slot snapshot and a confidence check against your own
 *    certainty about the intended target. When uncertain: read, do not write.
 *    When in doubt: surface to supervisor, do not proceed.
 *
 * 7. Do not call posix_madvise() (especially DONTNEED) on a shared store
 *    directly. Go through the bid table (splinter_shard_claim + splinter_madvise)
 *    so the election can arbitrate. A stray DONTNEED can evict another agent's
 *    live working set; the soft bumper guards the worst case, not every case.
 *    Re-bid to extend a window — never hold sovereignty by squatting.
 *
 * 8. splinter_retrain_slot() rewinds a slot's epoch and scrubs its vector.
 *    Call it only when you own the training lifecycle for that key — a backward
 *    epoch tells every watcher to discard what it cached. It is the right tool
 *    to release a seqlock left stuck odd by a crashed trainer, and the wrong
 *    tool for an ordinary refresh (use splinter_set / splinter_append instead).
 */

/**
 * @brief Copy the current atomic Splinter slot header to a corresponding client
 * structure.
 * @param snapshot A splinter_slot_snaphshot_t structure to receive the values.
 * @return -1 on failure, 0 on success.
 */
int splinter_get_slot_snapshot(const char *key, splinter_slot_snapshot_t *snapshot);

/**
 * @brief Creates and initializes a new splinter store.
 * @param name_or_path The name of the shared memory object or path to the file.
 * @param slots The total number of key-value slots to allocate.
 * @param max_value_sz The maximum size in bytes for any single value.
 * @return 0 on success, -1 on failure (e.g., store already exists).
 * @note Creation is exclusive (O_EXCL): if a store of the same name/path already
 *       exists this fails (errno EEXIST) rather than adopting or reinitializing
 *       it. In persistent mode the path is also opened O_NOFOLLOW, so a symlink
 *       planted at that path is refused. Use splinter_open() to attach to an
 *       existing store, or splinter_create_or_open() to do either.
 * @note The store is created with mode 0666 masked by the process umask, so by
 *       default it inherits the shell's umask (often world-readable on GNU
 *       systems). Set SPLINTER_DEFAULT_UMASK in the environment to an octal mask
 *       (e.g. "077" for a private 0600 store) to override this at creation time;
 *       adjust further with chmod afterward. Applies to persistent mode too.
 */
int splinter_create(const char *name_or_path, size_t slots, size_t max_value_sz);

/**
 * @brief Opens an existing splinter store.
 * @param name_or_path The name of the shared memory object or path to the file.
 * @return 0 on success, -1 on failure (e.g., store does not exist).
 */
int splinter_open(const char *name_or_path);

#ifdef SPLINTER_NUMA_AFFINITY
/**
 * @brief Opens the Splinter bus and binds it to a specific NUMA node.
 * This ensures all memory pages for the VALUES arena and slots 
 * stay local to the target socket's memory controller.
 */
void* splinter_open_numa(const char *name, int target_node);
#endif // SPLINTER_NUMA_AFFINITY

/**
 * @brief Opens an existing splinter store, or creates it if it does not exist.
 * @param name_or_path The name of the shared memory object or path to the file.
 * @param slots The total number of key-value slots if creating.
 * @param max_value_sz The maximum value size in bytes if creating.
 * @return 0 on success, -1 on failure.
 */
int splinter_open_or_create(const char *name_or_path, size_t slots, size_t max_value_sz);

/**
 * @brief Creates a new splinter store, or opens it if it already exists.
 * @param name_or_path The name of the shared memory object or path to the file.
 * @param slots The total number of key-value slots if creating.
 * @param max_value_sz The maximum value size in bytes if creating.
 * @return 0 on success, -1 on failure.
 */
int splinter_create_or_open(const char *name_or_path, size_t slots, size_t max_value_sz);

/**
 * @brief Closes the splinter store and unmaps the shared memory region.
 */
void splinter_close(void);

// About Splinter "Mop" Modes
// Because splinter has static geometry, there's no 'row level' cleanup required.
// We only have key -> value, where value can be up to max_val_sz.
//
// 99.999% of people will never have to think about this. Unless you're doing LLM 
// training, high-signal runtimes, or verifiable scientific research, you can 
// probably ignore the sanitation stuff.
//
// If your store has a max_val_sz of 1024, and you always write 1024 bytes, 
// there's no chance old data could creep into new reads. However, if your 
// max len is 1024 and you first write 900 bytes, then later only 100 bytes, 
// a reader using raw pointers (bypassing the library's length-checking) 
// stands a chance of over-reading up to 800 bytes of stale data.
//
// To prevent this while respecting the "Centerline" of performance, it offers
// three modes of "Auto Scrubbing":
//
// 0. None: Behavior similar to a file system. Fastest throughput
//    (3.3M+ ops/sec on old HW) with zero energetic waste.
//
// 1. Hybrid (Fast Mop, Default): Zero out the incoming length plus a 64-byte aligned
//    "slop" region. This prevents SIMD/Vectorized loads from seeing stale 
//    data without the cost of a full boil.
//
// 2. Full (Boil): Zero out the entire max_val_sz assigned to that slot. 
//    This ensures absolute hygiene for LLM memory and forensics, but 
//    it "squats" on the seqlock longer.
//
// Hybrid is more than sufficient for most needs (and is the default for 
// MRSW stress tests if scrubbing is enabled). Full boil is only recommended 
// if you ABSOLUTELY require verifiable zero-contamination.
//
// Purge: Can be run during backfill runs or maintenance to zero out lingering 
// orphan data in empty slots or active tails. This doesn't reclaim space; 
// it only ensures the manifold is clean.

/**
 * @brief Control Splinter's mop mode. 
 * @param unsigned mode:  0 = off, 1 = hybrid, 2 = full boil.
 * @return 0 on success, -1 on invalid mode, -2 if something is wrong with the store
 * This will replace all _av() functions.
 */
int splinter_set_mop(unsigned int mode);

/**
 * @brief Get the current "mop mode"
 * @return 0 = off, 1 = hybrid, 2 = full boil. -2 = no store.
 */
int splinter_get_mop(void);

/**
 * @brief Check each key, and zero out memory past the value length to the 
 * allocated slot length (essentially sweep out any old data). Designed to be
 * used as part of backfill runs when I/O slamming has stopped.
 */
void splinter_purge(void);

/**
 * @brief Sets or updates a key-value pair in the store.
 * @param key The null-terminated key string.
 * @param val Pointer to the value data.
 * @param len The length of the value data. Must not exceed `max_val_sz`.
 * @return 0 on success, -1 on failure (e.g., store is full).
 */
int splinter_set(const char *key, const void *val, size_t len);

/**
 * @brief "unsets" a key. 
 * This function does one atomic operation to zero the slot hash, which marks the
 * slot available for write. It then zeroes out the used key and value regions,
 * and resets the slot.
 *
 * @param key The null-terminated key string.
 * @return length of value deleted, -1 if key not found, - 2 if null key/store
 */
int splinter_unset(const char *key);

/**
 * @brief Retrieves the value associated with a key.
 * @param key The null-terminated key string.
 * @param buf The buffer to copy the value data into. Can be NULL to query size.
 * @param buf_sz The size of the provided buffer.
 * @param out_sz Pointer to a size_t to store the value's actual length. Can be NULL.
 * @return 0 on success, -1 on failure. If buf_sz is too small, sets errno to EMSGSIZE.
 */
int splinter_get(const char *key, void *buf, size_t buf_sz, size_t *out_sz);

/**
 * @brief Lists all keys currently in the store.
 * @param out_keys An array of `char*` to be filled with pointers to the keys.
 * @param max_keys The maximum number of keys to write to `out_keys`.
 * @param out_count Pointer to a size_t to store the number of keys found.
 * @return 0 on success, -1 on failure.
 */
int splinter_list(char **out_keys, size_t max_keys, size_t *out_count);

/**
 * @brief Waits for a key's value to be changed.
 * @param key The key to monitor for changes.
 * @param timeout_ms The maximum time to wait in milliseconds.
 * @return 0 if the value changed, -1 on timeout or if the key doesn't exist.
 */
int splinter_poll(const char *key, uint64_t timeout_ms);

#ifdef SPLINTER_EMBEDDINGS
/**
 * @brief Sets the embedding for a specific key.
 * @param key The null-terminated key string.
 * @param embedding Pointer to an array of 768 floats.
 * @return 0 on success, -1 on failure.
 */
int splinter_set_embedding(const char *key, const float *embedding);

/**
 * @brief Retrieves the embedding for a specific key.
 * @param key The null-terminated key string.
 * @param embedding_out Pointer to a buffer to store 768 floats.
 * @return 0 on success, -1 on failure.
 */
int splinter_get_embedding(const char *key, float *embedding_out);
#endif // SPLINTER_EMBEDDINGS

/**
 * @brief Set a bus configuration value 
 * @param hdr: a splinter  bus header structure
 * @param mask: bitmask to apply
 */
void splinter_config_set(struct splinter_header *hdr, uint8_t mask);

/**
 * @brief Clear a bus configuration value 
 * @param hdr: a splinter  bus header structure
 * @param mask: bitmask to clear
 */
void splinter_config_clear(struct splinter_header *hdr, uint8_t mask);

/**
 * @brief Test a bus configuration value 
 * @param hdr: a splinter  bus header structure
 * @param mask: bitmask to test
 */
int splinter_config_test(struct splinter_header *hdr, uint8_t mask);

/**
 * @brief Snapshot a bus configuration 
 * @param hdr: a splinter  bus header structure
 */
uint8_t splinter_config_snapshot(struct splinter_header *hdr);

/**
 * @brief Set a user slot flag 
 * @param slot Splinter slot structure
 * @param mask bitmask to set
 */
void splinter_slot_usr_set(struct splinter_slot *slot, uint16_t mask);

/**
 * @brief Clear a user slot flag 
 * @param slot Splinter slot structure
 * @param mask bitmask to clear
 */
void splinter_slot_usr_clear(struct splinter_slot *slot, uint16_t mask);

/**
 * @brief Test a user slot flag 
 * @param slot Splinter slot structure
 * @param mask bitmask to test
 */
int splinter_slot_usr_test(struct splinter_slot *slot, uint16_t mask);

/**
 * @brief Get a user slot flag snapshot 
 * @param slot Splinter slot structure
 */
uint16_t splinter_slot_usr_snapshot(struct splinter_slot *slot);

/**
 * @brief Name (declare intent to) a type fo a slot
 * @param key Name of the key to change
 * @param mask Splinter type bitmask to apply (e.g SPL_SLOT_TYPE_BIGUINT)
 * @return -1 or on error (sets errno), 0 on success
 */
int splinter_set_named_type(const char *key, uint16_t mask);

/**
 * @brief A helper to get the 64-bit cycle counter in order to 
 * set a demarcation point for elapsed time to calculate jitter 
 * in timestamp backfill. Accessing a wall clock isn't something
 * we can reasonably do in a seqlock; so we backfill the ctime
 * and atime stamps only if we need them.
 * 
 * waypoint = spointer_now();
 * splinter_set("foo", value);
 * time_t time = time(NULL); // syscalls take time (har har har)
 * now = splinter_now();
 * splinter_set_slot_time("foo", SPL_TIME_CTIME, time, now - waypoint);
 * 
 * The result is a timestamp that's more accurate than had the
 * syscall happened during (or before) the write, so it's preferable,
 * if also a tiny bit imperfect.
 * @return uint64
 */
inline uint64_t splinter_now(void) {
#if defined(__x86_64__) || defined(__i386__)
    // rdtsc: cycle counter, non-invariant on older throttled CPUs
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    // virtual counter; fixed-frequency, userspace-accessible on Linux
    uint64_t val;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#elif defined(__arm__)
    // 64-bit physical counter; may be trapped+emulated on older kernels
    uint32_t lo, hi;
    __asm__ __volatile__ ("mrrc p15, 1, %0, %1, c14" : "=r" (lo), "=r" (hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/**
 * @brief Update a slot's ctime / atime
 * @param key Name of the key to change
 * @param mode (SPL_TIME_CTIME or SPL_TIME_ATIME)
 * @param epoch client-supplied timestamp
 * @param offset value to subtract from epoch due to update-after-write
 * @return -1/-2 or on error (sets errno), 0 on success
 */
int splinter_set_slot_time(const char *key, unsigned short mode, 
    uint64_t epoch, size_t offset);

/**
 * @brief Bitwise & arithmetic ops on keys named as big unsigned
 * @param key Name of the key  to operate on
 * @param op Operation you want to do
 * @param mask What you want to do it with
 * @return 0 on success, -1 / -2 on internal / caller errors respectively
 */
int splinter_integer_op(const char *key, splinter_integer_op_t op, const void *mask);

/**
 * @brief Get a direct pointer to a value in shared memory.
 * @warning Unsafe: The data at this pointer can change or be zeroed if a 
 * writer modifies the slot. Use splinter_get_epoch to verify consistency.
 * @param key The key to look up.
 * @param out_sz Pointer to receive the actual length of the value.
 * @param out_epoch Pointer to receive the epoch at the time of lookup.
 * @return A const pointer to the data in SHM, or NULL if not found.
 */
const void *splinter_get_raw_ptr(const char *key, size_t *out_sz, uint64_t *out_epoch);

/**
 * @brief Get the current epoch of a specific slot.
 * @return The 64-bit epoch, or 0 if key not found.
 */
uint64_t splinter_get_epoch(const char *key);

/**
 * @brief Advance the epoch of a slot without otherwise doing work
 * Useful in conjunction with labeling for automation to fire.
 * @param key Current key name associated with the slot.
 */
int splinter_bump_slot(const char *key);

/**
 * @brief Reset a slot for retraining: scrub its vectors and republish.
 *
 * Forcibly zeroes the slot's embedding (when compiled with embeddings) and
 * drives the seqlock back to a known-good even epoch of 4, releasing any
 * sequence lock that may be stuck odd from a dead or aborted trainer. The
 * epoch is stored outright rather than advanced, so this succeeds regardless
 * of the slot's current state. Watchers and the event bus are pulsed so the
 * change is republished.
 *
 * The epoch moving *backwards* is the documented signal to clients and
 * watchers that the key must be revalidated. Works even when embeddings are
 * not compiled in; in that case it simply resets the epoch and republishes.
 *
 * @param key Current key name associated with the slot.
 * @return 0 on success, -1 if key not found, -2 on bad arguments.
 */
int splinter_retrain_slot(const char *key);

/**
 * @brief Atomically apply a label mask to a slot's Bloom filter.
 * @return 0 on success, -1 if key not found.
 */
int splinter_set_label(const char *key, uint64_t mask);

/**
 * @brief Atomically remove a previously applied bloom label
 * @return 0 on success, negative on failure
 */
int splinter_unset_label(const char *key, uint64_t mask);

/**
 * @brief Client-side helper to write multiple orders of a key.
 * This helper manages the naming convention for the caller.
 * It uses a temporary array to copy the "victim" keys.
 * @param base_key The main key (e.g. car)
 * @param vals An array of values from keys that will be merged in
 * @param lens An array of lengths corresponding with vals
 * @param orders How many tandems to create
 * @return 0 on success, -1 on failure, -2 if underlying basic I/O calls fail
 */
int splinter_client_set_tandem(const char *base_key, const void **vals, 
                               const size_t *lens, uint8_t orders);

/**
 * @brief Client-side helper to delete a key and its known orders.
 */
void splinter_client_unset_tandem(const char *base_key, uint8_t orders);

/**
 * @brief Registers interest in a key's group signal.
 */
int splinter_watch_register(const char *key, uint8_t group_id);

/**
 * @brief Unregisters interest in a key's group signal.
 */
int splinter_watch_unregister(const char *key, uint8_t group_id);

/**
 * @brief Maps a Bloom label (bitmask) to a signal group.
 */
int splinter_watch_label_register(uint64_t bloom_mask, uint8_t group_id);

/**
 * @brief Internal helper to pulse the Signal Arena for a slot.
 * @param slot pointer to a splinter_slot structure
 */
void splinter_pulse_watchers(struct splinter_slot *slot);

/**
 * @brief Pulse a key group by one of its members (if known)
 * @param key string key to find
 * @return 0 on success, -2 on system failure, -1 if key is not found
 */
int splinter_pulse_keygroup(const char *key);

/**
 * @brief Safely retrieve the current pulse count for a signal group. Good for debugging.
 * @param group_id The signal group (0-63).
 * @return The 64-bit pulse count, or 0 if invalid.
 */
uint64_t splinter_get_signal_count(uint8_t group_id);

/**
 * @brief Iterates through all slots matching a bloom mask.
 * @param mask The bloom mask to match against.
 * @param callback Function to call for each match.
 * @param user_data Opaque pointer for the callback.
 */
void splinter_enumerate_matches(uint64_t mask,
    void (*callback)(const char *key, uint64_t version, void *data), void *user_data);

/**
 * @brief Initialize the event bus (owner process only).
 *
 * Creates an eventfd, stores the owner PID and fd number in the shared header,
 * and arms the process-local write fd used by all subsequent write operations.
 * Call once per store lifetime, from the process that creates or governs the bus.
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int splinter_event_bus_init(void);

/**
 * @brief Open a process-local read fd to the owner's eventfd.
 *
 * Reads owner_pid and owner_fd from the shared header and opens
 * /proc/<owner_pid>/fd/<owner_fd> to obtain a local file descriptor
 * pointing to the same kernel eventfd object.  Works both in the owner
 * process and in any other process that has the store mapped.
 *
 * @return A valid fd on success (caller must pass it to splinter_event_bus_close),
 *         or -1 on failure (errno set).
 */
int splinter_event_bus_open(void);

/**
 * @brief Block until the global epoch changes or the timeout expires.
 *
 * Uses poll(2) + read(2) on the fd returned by splinter_event_bus_open().
 * On return, the eventfd counter has been drained; the caller should call
 * splinter_event_bus_get_dirty() or splinter_event_bus_take_dirty() to find
 * which slot stripes changed.
 *
 * The underlying eventfd is non-blocking, and O_NONBLOCK lives on the open file
 * description shared by every fd handed out for this bus. So if two waiters
 * poll concurrently and one drains the counter first, the other's read fails
 * with EAGAIN and this returns -1 rather than blocking indefinitely. Treat a -1
 * as "nothing for me right now" and re-arm; it is not necessarily an error.
 *
 * @param fd       The fd returned by splinter_event_bus_open().
 * @param timeout_ms Maximum wait time in milliseconds; 0 = non-blocking,
 *                   UINT64_MAX = wait forever.
 * @return 0 if a change was detected, -1 on timeout or error.
 */
int splinter_event_bus_wait(int fd, uint64_t timeout_ms);

/**
 * @brief Close a fd obtained from splinter_event_bus_open().
 * @param fd The fd to close.
 */
void splinter_event_bus_close(int fd);

/**
 * @brief Copy a snapshot of the dirty-slot bitmask into caller-supplied storage.
 *
 * Bit i of word w covers physical slots [b*stripe, (b+1)*stripe) where
 * b = w*64 + i and stripe = splinter_event_bus_stripe().  A set bit means at
 * least one slot in that contiguous range was written since the bus was
 * initialized.  For stores of SPLINTER_EVENT_BUS_BITS slots or fewer the stripe
 * is 1 and bit b is exactly slot b.
 *
 * This call does NOT clear the mask, so diff the snapshot against your own
 * saved copy to find newly-dirtied stripes.  If you do not diff, the mask
 * saturates to all-ones over the life of the store.  A single consumer that has
 * no need to share the mask should use splinter_event_bus_take_dirty() instead.
 *
 * @param out   Destination array; must hold at least `words` uint64_t values.
 * @param words Number of words to copy (cap: SPLINTER_EVENT_BUS_MASK_WORDS).
 */
void splinter_event_bus_get_dirty(uint64_t *out, size_t words);

/**
 * @brief Snapshot the dirty-slot bitmask and clear it in one atomic step.
 *
 * Same bit geometry as splinter_event_bus_get_dirty(), but each word is read
 * and zeroed atomically, so every set bit is reported exactly once and the mask
 * cannot saturate.  This is the right call for a single consumer.
 *
 * Destructive to other watchers: bits taken here are gone for everyone.  With
 * two or more independent readers of the same store, use
 * splinter_event_bus_get_dirty() and have each reader diff its own copy.
 *
 * @param out   Destination array; must hold at least `words` uint64_t values.
 * @param words Number of words to take (cap: SPLINTER_EVENT_BUS_MASK_WORDS).
 */
void splinter_event_bus_take_dirty(uint64_t *out, size_t words);

/**
 * @brief Number of physical slots covered by each dirty-mask bit.
 *
 * Returns ceil(slots / SPLINTER_EVENT_BUS_BITS) for the currently mapped store,
 * or 1 if no store is mapped.  Bit b of the mask covers physical slots
 * [b*stripe, min((b+1)*stripe, slots)).  A return of 1 means the mask is exact.
 *
 * @return Slots per dirty-mask bit; never 0.
 */
size_t splinter_event_bus_stripe(void);

/**
 * @brief Promotes a key to "system" usage
 * @param key the key to scope
 */
int splinter_set_as_system(const char *key);

/**
 * @brief Appends data to an existing key's value in-place.
 * @param key      The null-terminated key string.
 * @param data     Pointer to the data to append.
 * @param data_len Number of bytes to append.
 * @param new_len  Output: set to the new total value length on success. May be NULL.
 * @return 0 on success, -1 if key not found or overflow, -2 if args invalid.
 */
int splinter_append(const char *key, const void *data, size_t data_len, size_t *new_len);

/* ---------------------------------------------------------------------------
 * Logic Shard Election & Voluntary Yield (cooperative memory advisement).
 *
 * A fixed 32-slot bid table in the header lets independently-loaded shards
 * declare their memory intent (WILLNEED / SEQUENTIAL / RANDOM / DONTNEED) and
 * cooperatively decide who gets to issue posix_madvise() for a region. The
 * election is a read-only O(32) scan deterministic from static bid data, so
 * every shard computes the same sovereign without a central arbiter.
 *
 * Return-code convention: 0 success, -1 recoverable/contested (errno set,
 * e.g. EAGAIN / ENOSPC / ETIMEDOUT), -2 caller error (NULL/invalid arg or no
 * store mapped).
 * ------------------------------------------------------------------------- */

/**
 * @brief Claim a shard bid slot and declare memory intent.
 *
 * CAS-claims the first empty slot (or refreshes the caller's existing slot if
 * shard_id already owns one), stamping pid=getpid() and claimed_at=splinter_now().
 *
 * @param shard_id     Caller-chosen non-zero shard identifier (unique per process).
 * @param intent       splinter_intent_t advisement class.
 * @param priority     0-255, higher wins elections.
 * @param duration_tsc Declared sovereignty window in splinter_now() ticks.
 * @return 0 on success, -1 if the table is full (errno=ENOSPC),
 *         -2 on bad args / no store / shard_id==0.
 *
 * @note On a fresh claim the descriptive fields are published with
 *       memory_order_release *after* the shard_id CAS makes the slot visible.
 *       A racing election that observes the new shard_id but not-yet-stored
 *       fields treats the bid as SPL_INTENT_NONE / priority 0 for at most one
 *       election; the next election corrects it. Advisement is a hint, so this
 *       is acceptable by design.
 */
int splinter_shard_claim(uint32_t shard_id, uint8_t intent,
                         uint8_t priority, uint64_t duration_tsc);

/**
 * @brief Advanced/testing claim: explicit pid and claimed_at.
 *
 * Identical to splinter_shard_claim() but lets a supervisor register a bid on
 * behalf of another process, and lets tests construct deterministic election
 * scenarios without sleeping. Production shards should prefer splinter_shard_claim().
 * @return as splinter_shard_claim().
 */
int splinter_shard_claim_ex(uint32_t shard_id, uint32_t pid, uint8_t intent,
                            uint8_t priority, uint64_t duration_tsc,
                            uint64_t claimed_at);

/**
 * @brief Refresh (re-bid) an existing claim's window. Updates claimed_at to
 * splinter_now() and optionally changes intent/priority/duration. This is the
 * fairness re-bid: a shard that needs more time must re-bid rather than hold.
 * @return 0 on success, -1 if shard_id holds no slot, -2 on bad args.
 */
int splinter_shard_rebid(uint32_t shard_id, uint8_t intent,
                         uint8_t priority, uint64_t duration_tsc);

/**
 * @brief Voluntarily release (yield) the caller's bid slot. Zeroes shard_id
 * (marking the slot empty) last, after clearing the other fields.
 * @return 0 on success, -1 if shard_id holds no slot, -2 on bad args/no store.
 */
int splinter_shard_release(uint32_t shard_id);

/**
 * @brief Run the read-only election scan and return the current sovereign.
 *
 * Highest-priority UNEXPIRED bid wins; ties broken by earliest claimed_at,
 * then lowest pid. DONTNEED soft bumper: a DONTNEED bid is skipped as a winner
 * while any unexpired WILLNEED or SEQUENTIAL bid exists.
 *
 * @param out_intent Optional; receives the winning bid's intent (or SPL_INTENT_NONE).
 * @return the winning shard_id, or 0 if there is no current sovereign.
 */
uint32_t splinter_shard_election(uint8_t *out_intent);

/**
 * @brief Convenience: is shard_id the current sovereign?
 * @return 1 if sovereign, 0 if not (including unknown/expired), -2 on no store.
 */
int splinter_shard_is_sovereign(uint32_t shard_id);

/**
 * @brief Copy a non-atomic snapshot of every bid slot for inspection/audit.
 * @param out   Array of at least SPLINTER_MAX_SHARDS records.
 * @param max   Capacity of out (capped at SPLINTER_MAX_SHARDS).
 * @return number of records copied, or -2 on bad args/no store.
 */
int splinter_shard_table_snapshot(struct splinter_shard_bid_snapshot *out, size_t max);

/**
 * @brief Cooperative posix_madvise(): the voluntary-yield entry point.
 *
 * Runs the election. If shard_id is sovereign, issues posix_madvise(addr,len,advice)
 * immediately and returns its result. If not sovereign:
 *   - timeout_ticks == 0          -> do NOT block; return -1, errno=EAGAIN (defer).
 *   - timeout_ticks == UINT64_MAX -> block until sovereign, then advise.
 *   - else                        -> block up to timeout_ticks, re-electing on each wake.
 * Blocking uses the eventfd broker when the bus owner has armed it
 * (splinter_event_bus_open/wait); otherwise a TSC-polled nanosleep fallback.
 *
 * If addr==NULL, advises the whole value arena (VALUES .. VALUES+arena_sz).
 *
 * @return 0 on success (advisement issued), -1 on EAGAIN/timeout/posix_madvise
 *         failure (errno set), -2 on bad args/no store/unknown shard_id.
 */
int splinter_madvise(uint32_t shard_id, void *addr, size_t len,
                     int advice, uint64_t timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif // SPLINTER_H
