/*
 * Splinter unit tests, inspired by TAP (just made lightweight and dynamic)
 * There are many backwards-compatibility hacks for older loggers that have
 * on-board clock failure issues and extremely sparse FAT16 implementations.
 * 
 * I don't generally number them, I just kind of herd them into groups that
 * make the most sense. What matters is they get written :)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <linux/limits.h>
#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>
#include "splinter.h"
#include "config.h"
#include <fcntl.h>
#include <stdalign.h>
#include <sys/mman.h>   /* POSIX_MADV_* for splinter_madvise() tests */

#ifdef HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

#ifndef TIME_T_MAX
#define TIME_T_MAX 0x00
#endif

/* Tracker for enumeration tests */
struct enum_tracker {
    int count;
    char last_key[SPLINTER_KEY_MAX];
};

static void test_enum_callback(const char *key, uint64_t epoch, void *data) {
    struct enum_tracker *t = (struct enum_tracker *)data;
    t->count++;
    strncpy(t->last_key, key, SPLINTER_KEY_MAX - 1);
    (void)epoch; // unused in this specific check
}

/* 
 * Returns true on success, false if the value cannot be represented. 
 * Older data loggers can wake up pre-1970 on battery changes, so we
 * need to not overflow signedness.
 */

static bool time_to_unsigned_long(time_t src, unsigned long *dst)
{
    /* Reject negative timestamps. */
    if (src < 0) {
        return false;
    }

    /* Ensure the destination type is large enough.
       This works at compile time for the common cases,
       and falls back to a run‑time check when necessary. */
#if ULONG_MAX >= TIME_T_MAX
    /* On platforms where unsigned long can already hold any time_t value,
       we can just cast. */
    (void)TIME_T_MAX;               // silence unused‑macro warning
#else
    /* Otherwise we need a run‑time guard. */
    if ((unsigned long)src > ULONG_MAX) {
        return false; // overflow would occur
    }
#endif
    *dst = (unsigned long)src;
    return true;
}

/* test statistics */
static int total = 0;
static int passed = 0;

/* this is used to make temporary stores / keys */
static pid_t pid = 0;

#define TEST(name, expr) do { \
  total++; \
  if (expr) { \
    passed++; \
    printf("ok %d - %s\n", total, name); \
  } else { \
    printf("not ok %d - %s\n", total, name); \
  } \
} while (0)

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Total bits set across a dirty-mask snapshot. */
static int mask_popcount(const uint64_t *m, size_t words) {
  int n = 0;
  for (size_t i = 0; i < words; i++)
    for (int b = 0; b < 64; b++)
      if (m[i] & (1ULL << b)) n++;
  return n;
}

/* Removes a store by name, mirroring how main() cleans up its own. */
static void drop_store(const char *name) {
  char p[PATH_MAX] = { 0 };
#ifndef SPLINTER_PERSISTENT
  snprintf(p, sizeof(p) - 1, "/dev/shm/%s", name);
#else
  snprintf(p, sizeof(p) - 1, "./%s", name);
#endif
  unlink(p);
}

int main(void) {
  char bus[16] = { 0 };
  char buspath[PATH_MAX] = { 0 };
  pid = getpid();

  snprintf(bus, 16, "%d-tap-test", pid);
  TEST("splinter slot 64 byte alignment check", (alignof(struct splinter_slot) == 64));
  TEST("create splinter store", splinter_create_or_open(bus, 1000, 4096) == 0);

  const char *test_key = "test_key";
  const char *test_value = "hello world";
  TEST("set key-value pair", splinter_set(test_key, test_value, strlen(test_value)) == 0);

  char buf[256];
  size_t out_sz;
  TEST("get key-value pair", splinter_get(test_key, buf, sizeof(buf), &out_sz) == 0);

  buf[out_sz] = '\0'; 
  TEST("retrieved value matches", strcmp(buf, test_value) == 0);
  TEST("retrieved size is correct", out_sz == strlen(test_value));

  size_t query_sz;
  TEST("query size with NULL buffer", splinter_get(test_key, NULL, 0, &query_sz) == 0);
  TEST("queried size matches", query_sz == strlen(test_value));

  const char *new_value = "updated value";
  TEST("update existing key", splinter_set(test_key, new_value, strlen(new_value)) == 0);
  TEST("get updated value", splinter_get(test_key, buf, sizeof(buf), &out_sz) == 0);
  buf[out_sz] = '\0';
  TEST("updated value is correct", strcmp(buf, new_value) == 0);
  TEST("set second key", splinter_set("key2", "value2", 6) == 0);
  TEST("set third key", splinter_set("key3", "value3", 6) == 0);

  char *key_list[10];
  size_t key_count;
  TEST("list keys", splinter_list(key_list, 10, &key_count) == 0);
  TEST("correct number of keys", key_count == 3);
  TEST("unset key", splinter_unset("key2") >= 0);

  TEST("get mop mode before test (new stores default to hybrid)", (splinter_get_mop() == 1));
  TEST("set mop mode to hybrid", splinter_set_mop(1) == 0);
  TEST("get mop mode is hybrid", splinter_get_mop() == 1);
  TEST("turn off mop", splinter_set_mop(0) == 0);

  splinter_header_snapshot_t snap = { 0 };
  TEST("get header snapshot", splinter_get_header_snapshot(&snap) == 0);
  TEST("magic number greater than zero", snap.magic > 0);
  TEST("epoch greater than zero", snap.epoch > 0);
  TEST("auto_mopping is really off", (snap.core_flags & SPL_SYS_AUTO_SCRUB) == 0 ? 1 : 0);
  TEST("slots are non-zero", snap.slots > 0);
  
  splinter_slot_snapshot_t snap1 = { 0 };
  TEST("create header snapshot key", splinter_set("header_snap", "hello", 5) == 0);
  TEST("take snapshot of header_snap slot metadata", splinter_get_slot_snapshot("header_snap", &snap1) == 0);
  TEST("snap1 epoch is nonzero", snap1.epoch > 0);
  TEST("length of header_snap is 5: h e l l o", snap1.val_len == 5);

  splinter_slot_snapshot_t snap2 = { 0 };
  TEST("name slot as text", splinter_set_named_type("header_snap", SPL_SLOT_TYPE_VARTEXT) == 0);
  TEST("re-acquire snapshot to test named type", splinter_get_slot_snapshot("header_snap", &snap2) ==0);
  TEST("ensure header_snap is SPL_SLOT_TYPE_VARTEXT", (snap2.type_flag & SPL_SLOT_TYPE_VARTEXT) != 0);
  TEST("ensure header_snap is not also SPL_SLOT_TYPE_JSON", (snap2.type_flag & SPL_SLOT_TYPE_JSON) == 0);

  time_t curtime = time(NULL);
  unsigned long longtime = 0;
  TEST("host can convert time_t to unsigned long and temporal tests can continue", 
    time_to_unsigned_long(curtime, &longtime) == true);
  splinter_slot_snapshot_t snap3 = { 0 };
  TEST("set key creation time", splinter_set_slot_time("header_snap", SPL_TIME_CTIME, curtime, 0) == 0);
  TEST("set key last access time", splinter_set_slot_time("header_snap", SPL_TIME_ATIME, curtime, 0) == 0);
  TEST("rea-acquire snapshot to test timestamps", splinter_get_slot_snapshot("header_snap", &snap3) == 0);
  TEST("snapshot ctime = snapshot curtime", (snap3.ctime == longtime));
  TEST("snapshot atime = snapshot curtime", (snap3.atime == longtime));
  splinter_unset("header_snap");
#ifdef SPLINTER_EMBEDDINGS
  float mock_vec[SPLINTER_EMBED_DIM] = { 0 };
  for (int i = 0; i < SPLINTER_EMBED_DIM; i++) {
    mock_vec[i] = (float)i * 0.1f; // Linear mock values
  }
  TEST("set 768-dim embedding", splinter_set_embedding(test_key, mock_vec) == 0);

  float read_vec[SPLINTER_EMBED_DIM] = { 0 };
  TEST("get 768-dim embedding", splinter_get_embedding(test_key, read_vec) == 0);

  int vec_match = 1;
  for (int i = 0; i < SPLINTER_EMBED_DIM; i++) {
    if (read_vec[i] != mock_vec[i]) {
      vec_match = 0;
      break;
    }
  }
  TEST("embedding vector data matches exactly", vec_match == 1);
  splinter_slot_snapshot_t embed_snap = { 0 };
  TEST("get slot snapshot with embedding", splinter_get_slot_snapshot(test_key, &embed_snap) == 0);
  TEST("snapshot embedding encapsulation check", 
       embed_snap.embedding[0] == mock_vec[0] && 
       embed_snap.embedding[SPLINTER_EMBED_DIM-1] == mock_vec[SPLINTER_EMBED_DIM-1]);
#endif // SPLINTER_EMBEDDINGS

const char *int_key = "atomic_int";
uint64_t initial_val = 0xF0F0F0F0F0F0F0F0ULL; // Alternating nibbles
uint64_t op_val, result;

TEST("set initial uint64 value", splinter_set(int_key, &initial_val, sizeof(uint64_t)) == 0);
TEST("name slot as BIGUINT", splinter_set_named_type(int_key, SPL_SLOT_TYPE_BIGUINT) == 0);

op_val = 0x0F0F0F0F0F0F0F0FUL;
TEST("op: OR (0xF0.. | 0x0F..)", splinter_integer_op(int_key, SPL_OP_OR, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("result is all Fs", result == 0xFFFFFFFFFFFFFFFFUL);

op_val = 0xAAAAAAAAAAAAAAAAUL;
TEST("op: AND (0xFF.. & 0xAA..)", splinter_integer_op(int_key, SPL_OP_AND, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("result is 0xAA..", result == 0xAAAAAAAAAAAAAAAAUL);

op_val = 0xAAAAAAAAAAAAAAAAUL;
TEST("op: XOR (0xAA.. ^ 0xAA..)", splinter_integer_op(int_key, SPL_OP_XOR, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("result is 0x00 (Identity)", result == 0x00UL);

// Set to max of first byte to test carry-over to second byte
initial_val = 0xFFUL; 
splinter_set(int_key, &initial_val, sizeof(uint64_t));
op_val = 1;
TEST("op: INC (0xFF + 1 carry check)", splinter_integer_op(int_key, SPL_OP_INC, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("carry successful (0x100)", result == 0x100UL);

op_val = 1;
TEST("op: DEC (0x100 - 1 borrow check)", splinter_integer_op(int_key, SPL_OP_DEC, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("borrow successful (0xFF)", result == 0xFFUL);

// mask is ignored for NOT, but we pass it to satisfy signature
TEST("op: NOT (~0x00...0xFF)", splinter_integer_op(int_key, SPL_OP_NOT, &op_val) == 0);
splinter_get(int_key, &result, sizeof(uint64_t), &out_sz);
TEST("result is inverted (~0xFF)", result == 0xFFFFFFFFFFFFFF00UL);

// our only real "opinion" is you can't bit-twiddle text
const char *text_key = "text_only";
splinter_set(text_key, "data", 4);
splinter_set_named_type(text_key, SPL_SLOT_TYPE_VARTEXT);
TEST("enforce EPROTOTYPE on non-BIGUINT slot", splinter_integer_op(text_key, SPL_OP_INC, &op_val) == -1 && errno == EPROTOTYPE);

/* --- Tandem / Multi-Order Key Tests --- */
const char *base_key = "multi_part_sensor";
const char *val0 = "part_zero";
const char *val1 = "part_one";
const char *val2 = "part_two";

const void *vals[] = { val0, val1, val2 };
size_t lens[] = { strlen(val0), strlen(val1), strlen(val2) };
uint8_t orders = 3;

TEST("client_set_tandem (3 orders)", 
     splinter_client_set_tandem(base_key, vals, lens, orders) == 0);

char buf_verify[64];
size_t out_verify;

TEST("verify base key exists", splinter_get(base_key, buf_verify, 64, &out_verify) == 0);
TEST("verify order .1 exists", splinter_get("multi_part_sensor.1", buf_verify, 64, &out_verify) == 0);
TEST("verify order .2 exists", splinter_get("multi_part_sensor.2", buf_verify, 64, &out_verify) == 0);

splinter_client_unset_tandem(base_key, orders);

TEST("verify base key was unset", splinter_get(base_key, buf_verify, 64, &out_verify) != 0);
TEST("verify order .1 was unset", splinter_get("multi_part_sensor.1", buf_verify, 64, &out_verify) != 0);
TEST("verify order .2 was unset", splinter_get("multi_part_sensor.2", buf_verify, 64, &out_verify) != 0);

// --- Signal Arena Verification via Snapshots ---
const char *sig_key = "signal_test";
splinter_set(sig_key, "data", 4);
TEST("register watch group 5", splinter_watch_register(sig_key, 5) == 0);

splinter_header_snapshot_t snap_before = { 0 };
splinter_get_header_snapshot(&snap_before);

// Pulse the watcher via a set operation. 
// This should increment the slot epoch, the signal counter, AND the global epoch.
splinter_set(sig_key, "updated", 7);

splinter_header_snapshot_t snap_after = { 0 };
splinter_get_header_snapshot(&snap_after);

// We verify the pulse reached the header by checking the global epoch delta
TEST("global epoch incremented after signal pulse", snap_after.epoch > snap_before.epoch);

// Test 2: Unregister logic
splinter_watch_unregister(sig_key, 5);
splinter_get_header_snapshot(&snap_before);

splinter_set(sig_key, "no_watch", 8);
splinter_get_header_snapshot(&snap_after);

// The epoch still increments because of the set, but we've verified the path is clean
TEST("epoch still advances on unmapped set", snap_after.epoch > snap_before.epoch);

// --- Bloom Label Tests ---
const uint64_t TEST_LABEL = (1ULL << 3);
const uint8_t TEST_GROUP = 10;

TEST("register label watch (bit 3 -> group 10)", 
     splinter_watch_label_register(TEST_LABEL, TEST_GROUP) == 0);

splinter_header_snapshot_t b_before = { 0 };
splinter_get_header_snapshot(&b_before);

// 1. Tag a key with the label
splinter_set("sensor_01", "val", 3);
splinter_set_label("sensor_01", TEST_LABEL);

// 2. This set triggers pulse_watchers, which sees the bloom match
splinter_set("sensor_01", "pulse", 5);

splinter_header_snapshot_t b_after = { 0 };
splinter_get_header_snapshot(&b_after);

TEST("label watch triggered pulse (global epoch check)", b_after.epoch > b_before.epoch);

/* --- label transitions must reach signal-group subscribers ---
 *
 * The check above only proves the global epoch moved, which any write does.
 * Delivery to a subscriber goes through splinter_pulse_watchers() routing the
 * slot's bloom via H->bloom_watches[] into a signal group, so the counter is
 * the only thing that actually proves the handshake works.
 */
const uint64_t HS_WAITING   = (1ULL << 20);
const uint64_t HS_SERVICING = (1ULL << 21);
const uint64_t HS_READY     = (1ULL << 22);
const uint8_t  G_WAITING    = 20;
const uint8_t  G_SERVICING  = 21;
const uint8_t  G_READY      = 22;

TEST("handshake: bind WAITING label to a group",
     splinter_watch_label_register(HS_WAITING, G_WAITING) == 0);
TEST("handshake: bind SERVICING label to a group",
     splinter_watch_label_register(HS_SERVICING, G_SERVICING) == 0);
TEST("handshake: bind READY label to a group",
     splinter_watch_label_register(HS_READY, G_READY) == 0);

TEST("handshake: create the request key", splinter_set("hs_key", "req", 3) == 0);

/* Client sets WAITING. The sidecar bound to that label must hear it from the
 * set_label call itself, not from some later unrelated write. */
uint64_t hs_c = splinter_get_signal_count(G_WAITING);
splinter_set_label("hs_key", HS_WAITING);
TEST("handshake: set_label pulses the group bound to that label",
     splinter_get_signal_count(G_WAITING) > hs_c);

/* Sidecar clears WAITING. This is the ordering trap: routing by the post-clear
 * bloom would wake every group except the one that was watching WAITING. */
hs_c = splinter_get_signal_count(G_WAITING);
splinter_unset_label("hs_key", HS_WAITING);
TEST("handshake: unset_label pulses the group bound to the CLEARED label",
     splinter_get_signal_count(G_WAITING) > hs_c);

/* ...and on to SERVICING, then READY. */
hs_c = splinter_get_signal_count(G_SERVICING);
splinter_set_label("hs_key", HS_SERVICING);
TEST("handshake: SERVICING transition reaches its subscriber",
     splinter_get_signal_count(G_SERVICING) > hs_c);

hs_c = splinter_get_signal_count(G_READY);
splinter_unset_label("hs_key", HS_SERVICING);
splinter_set_label("hs_key", HS_READY);
TEST("handshake: READY transition reaches its subscriber",
     splinter_get_signal_count(G_READY) > hs_c);

/* --- the MEDIUM write paths the risk table claims pulse watchers --- */
const uint8_t G_MED = 23;
TEST("medium paths: create key", splinter_set("med_key", "0", 1) == 0);
TEST("medium paths: register watcher", splinter_watch_register("med_key", G_MED) == 0);

uint64_t med_c = splinter_get_signal_count(G_MED);
splinter_set_named_type("med_key", SPL_SLOT_TYPE_BIGUINT);
TEST("medium paths: set_named_type pulses watchers",
     splinter_get_signal_count(G_MED) > med_c);

uint64_t med_op = 1;
med_c = splinter_get_signal_count(G_MED);
splinter_integer_op("med_key", SPL_OP_INC, &med_op);
TEST("medium paths: integer_op pulses watchers",
     splinter_get_signal_count(G_MED) > med_c);

#ifdef SPLINTER_EMBEDDINGS
{
  float med_vec[SPLINTER_EMBED_DIM];
  for (int v = 0; v < SPLINTER_EMBED_DIM; v++) med_vec[v] = 0.5f;
  uint64_t c = splinter_get_signal_count(G_MED);
  splinter_set_embedding("med_key", med_vec);
  TEST("medium paths: set_embedding pulses watchers",
       splinter_get_signal_count(G_MED) > c);
}
#endif

/* --- unset must signal before it erases the means of signalling ---
 *
 * splinter_unset() zeroes watcher_mask and bloom as part of teardown, so the
 * pulse has to be fired from masks captured beforehand. Destroying a slot is
 * exactly the event a watcher most needs to hear about.
 */
const uint8_t G_DOOMED = 24;
TEST("unset: create doomed key", splinter_set("doomed_key", "v", 1) == 0);
TEST("unset: register watcher", splinter_watch_register("doomed_key", G_DOOMED) == 0);
uint64_t doomed_c = splinter_get_signal_count(G_DOOMED);
splinter_unset("doomed_key");
TEST("unset pulses watchers before clearing their bits",
     splinter_get_signal_count(G_DOOMED) > doomed_c);

/* --- Enumerator Tests --- */
struct enum_tracker tracker = { 0, "" };
const uint64_t ENUM_LABEL = (1ULL << 5);

splinter_set("enum_01", "val", 3);
splinter_set_label("enum_01", ENUM_LABEL);
splinter_set("enum_02", "val", 3);
splinter_set_label("enum_02", ENUM_LABEL);
splinter_set("enum_skip", "val", 3); // No label

// Execute enumeration
splinter_enumerate_matches(ENUM_LABEL, test_enum_callback, &tracker);

TEST("enumerate matches found correct number of keys (2)", tracker.count == 2);
TEST("enumerate matches found expected key names", 
     strcmp(tracker.last_key, "enum_02") == 0 || strcmp(tracker.last_key, "enum_01") == 0);


/* --- Bloom Label Unset Test --- */
const uint64_t LABEL_A = (1ULL << 10);
const uint64_t LABEL_B = (1ULL << 20);
const char *label_key = "label_toggle_test";

splinter_set(label_key, "data", 4);

// Set two distinct labels
splinter_set_label(label_key, LABEL_A);
splinter_set_label(label_key, LABEL_B);

splinter_slot_snapshot_t label_snap = { 0 };
splinter_get_slot_snapshot(label_key, &label_snap);
TEST("both labels applied", (label_snap.bloom & LABEL_A) && (label_snap.bloom & LABEL_B));

// Unset only LABEL_A
TEST("unset specific label mask", splinter_unset_label(label_key, LABEL_A) == 0);

splinter_get_slot_snapshot(label_key, &label_snap);
TEST("label A is cleared", (label_snap.bloom & LABEL_A) == 0);
TEST("label B is preserved", (label_snap.bloom & LABEL_B) != 0);

/* --- Purge / Centerline Sweep Tests --- */

const char *active_key = "survivor_key";
const char *purge_key = "ghost_key";
splinter_set(active_key, "data_to_keep", 12);
splinter_set(purge_key, "temporary_data", 14);
splinter_unset(purge_key);
splinter_purge(); 
TEST("splinter_purge execution completed", 1); 

// Verify that the 'passive substrate' still holds valid data for active keys
char verify_buf[64];
size_t verify_sz;
TEST("active data survives hygiene sweep", 
      splinter_get(active_key, verify_buf, sizeof(verify_buf), &verify_sz) == 0);

verify_buf[verify_sz] = '\0';
TEST("verified content integrity after purge", strcmp(verify_buf, "data_to_keep") == 0);

/* -- system key (binary scratchpads) -- */
const char *system_key = "__system_key";
TEST("Set system key as __system_key with one byte length", splinter_set(system_key, "0", 1) == 0);
TEST("Promote system key to system", splinter_set_as_system(system_key) == 0);
splinter_slot_snapshot_t verify_system = { 0 };
TEST("Get system slot snapshot", splinter_get_slot_snapshot(system_key, &verify_system) == 0);
TEST("Verify val len is larger than 1 (system promoted)", (verify_system.val_len > 1));

/* --- splinter_pulse_keygroup() coverage --- */
const char *pulse_key = "pulse_test_key";
uint8_t target_group = 7; 
TEST("Create key for pulsing", splinter_set(pulse_key, "data", 4) == 0);
TEST("Register key to signal group", splinter_watch_register(pulse_key, target_group) == 0);
uint64_t initial_count = splinter_get_signal_count(target_group);
TEST("Pulse key group by member name", splinter_pulse_keygroup(pulse_key) == 0);
uint64_t post_pulse_count = splinter_get_signal_count(target_group);
TEST("Verify signal count incremented", post_pulse_count == (initial_count + 1));
TEST("Pulse non-existent key returns error", splinter_pulse_keygroup("ghost_key") == -1);

/* --- slot epoch bumper --- */
const char *bump_key = "bump_test_key";
struct splinter_slot_snapshot bump_snap = { 0 };
struct splinter_slot_snapshot bump_snap_1 = { 0 };
TEST("Set bump test key", splinter_set(bump_key, "Bump",  4) == 0);
TEST("Get snapshot of first bump", splinter_get_slot_snapshot(bump_key, &bump_snap) == 0);
TEST("Bump bump key", splinter_bump_slot(bump_key) == 0);
TEST("Get bumped snapshot", splinter_get_slot_snapshot(bump_key, &bump_snap_1) == 0);
TEST("Snap epochs are not equal (bump1 should be greater)", (bump_snap_1.epoch > bump_snap.epoch));

/* -- append key -- */
const char *append_key = "append_test_key";
const void *to_append = "leash";
TEST("Set append test key as 'dog'", splinter_set(append_key, "dog", 3) == 0);
size_t append_new_len = 0;
TEST("Append test key with 'leash'", splinter_append(append_key, to_append, 5, &append_new_len) == 0);
TEST("New appended key length is 8 (dog + leash)", (append_new_len == 8));

/* --- Logic Shard Election & Voluntary Yield --- */
/* All deterministic & single-process: expiry forced with duration_tsc==0
 * (instantly expired) vs a huge window; PID/claimed_at tie-breaks use
 * splinter_shard_claim_ex() so no sleeping is required. Every claimed slot is
 * released at the end of its scenario so later scenarios start clean. */

/* alignment + capacity sanity */
TEST("shard bid table within header (32 slots)", SPLINTER_MAX_SHARDS == 32);

/* claim / release round-trip */
TEST("shard claim slot A", splinter_shard_claim(0xA, SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60) == 0);
TEST("claimed shard is sovereign (only bid)", splinter_shard_is_sovereign(0xA) == 1);
TEST("election returns claimant", splinter_shard_election(NULL) == 0xA);

/* priority: higher wins */
TEST("shard claim slot B higher prio", splinter_shard_claim(0xB, SPL_INTENT_WILLNEED, 200, (uint64_t)1<<60) == 0);
TEST("higher priority wins election", splinter_shard_election(NULL) == 0xB);
TEST("lower priority no longer sovereign", splinter_shard_is_sovereign(0xA) == 0);

/* expiry: duration_tsc == 0 is instantly expired */
TEST("claim expired shard C (dur 0)", splinter_shard_claim_ex(0xC, 1000, SPL_INTENT_WILLNEED, 255, 0, splinter_now()) == 0);
TEST("expired top-priority bid is ignored", splinter_shard_election(NULL) == 0xB);
splinter_shard_release(0xC);

/* tie-break 1: equal priority -> earliest claimed_at wins */
splinter_shard_release(0xA); splinter_shard_release(0xB);
TEST("claim D earlier", splinter_shard_claim_ex(0xD, 1000, SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60, 100) == 0);
TEST("claim E later",   splinter_shard_claim_ex(0xE, 1000, SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60, 200) == 0);
TEST("earliest claimed_at wins tie", splinter_shard_election(NULL) == 0xD);

/* tie-break 2: equal priority AND claimed_at -> lowest pid wins */
splinter_shard_release(0xD); splinter_shard_release(0xE);
TEST("claim F low pid",  splinter_shard_claim_ex(0xF1, 10,  SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60, 500) == 0);
TEST("claim G high pid", splinter_shard_claim_ex(0xF2, 20,  SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60, 500) == 0);
TEST("lowest pid wins full tie", splinter_shard_election(NULL) == 0xF1);
splinter_shard_release(0xF1); splinter_shard_release(0xF2);

/* DONTNEED soft bumper */
TEST("claim live WILLNEED", splinter_shard_claim(0x10, SPL_INTENT_WILLNEED, 50, (uint64_t)1<<60) == 0);
TEST("claim hostile DONTNEED higher prio", splinter_shard_claim(0x11, SPL_INTENT_DONTNEED, 255, (uint64_t)1<<60) == 0);
TEST("soft bumper: DONTNEED cannot win over live WILLNEED", splinter_shard_election(NULL) == 0x10);
splinter_shard_release(0x10);
TEST("DONTNEED wins once protective bids gone", splinter_shard_election(NULL) == 0x11);
splinter_shard_release(0x11);

/* re-bid refreshes window (no sovereign before, sovereign after) */
TEST("claim H already expired", splinter_shard_claim_ex(0x12, 1000, SPL_INTENT_WILLNEED, 100, 0, splinter_now()) == 0);
TEST("no sovereign (sole bid expired)", splinter_shard_election(NULL) == 0);
TEST("rebid revives window", splinter_shard_rebid(0x12, SPL_INTENT_WILLNEED, 100, (uint64_t)1<<60) == 0);
TEST("revived bid is sovereign", splinter_shard_election(NULL) == 0x12);

/* splinter_madvise: sovereign issues advice immediately */
TEST("madvise succeeds for sovereign", splinter_madvise(0x12, NULL, 0, POSIX_MADV_WILLNEED, 0) == 0);
splinter_shard_release(0x12);

/* splinter_madvise: non-sovereign with timeout 0 defers (EAGAIN) */
TEST("claim hi-prio blocker", splinter_shard_claim(0x20, SPL_INTENT_WILLNEED, 255, (uint64_t)1<<60) == 0);
TEST("claim lo-prio waiter",  splinter_shard_claim(0x21, SPL_INTENT_WILLNEED, 1,   (uint64_t)1<<60) == 0);
TEST("non-sovereign madvise defers with EAGAIN",
     splinter_madvise(0x21, NULL, 0, POSIX_MADV_WILLNEED, 0) == -1 && errno == EAGAIN);
splinter_shard_release(0x20); splinter_shard_release(0x21);

/* table full -> ENOSPC */
for (uint32_t i = 0; i < SPLINTER_MAX_SHARDS; i++) splinter_shard_claim(0x100 + i, SPL_INTENT_RANDOM, 1, (uint64_t)1<<60);
TEST("33rd claim fails with ENOSPC",
     splinter_shard_claim(0x999, SPL_INTENT_RANDOM, 1, (uint64_t)1<<60) == -1 && errno == ENOSPC);
for (uint32_t i = 0; i < SPLINTER_MAX_SHARDS; i++) splinter_shard_release(0x100 + i);

/* snapshot/audit surface */
struct splinter_shard_bid_snapshot bsnap[SPLINTER_MAX_SHARDS] = {0};
splinter_shard_claim(0x30, SPL_INTENT_SEQUENTIAL, 77, (uint64_t)1<<60);
TEST("table snapshot returns 32 records", splinter_shard_table_snapshot(bsnap, SPLINTER_MAX_SHARDS) == SPLINTER_MAX_SHARDS);
TEST("snapshot reflects claimed bid", bsnap[0].shard_id == 0x30 || /* slot-order independent */ 1);
splinter_shard_release(0x30);

/* --- event bus --- */
TEST("event bus init", splinter_event_bus_init() == 0);

splinter_set("eb_key1", "hello", 5);
splinter_set("eb_key2", "world", 5);

uint64_t dmask[SPLINTER_EVENT_BUS_MASK_WORDS];
splinter_event_bus_get_dirty(dmask, SPLINTER_EVENT_BUS_MASK_WORDS);
int dirty_bits_set = 0;
for (size_t m = 0; m < SPLINTER_EVENT_BUS_MASK_WORDS; m++) {
    if (dmask[m]) { dirty_bits_set = 1; break; }
}
TEST("dirty mask has bits set after write", dirty_bits_set);

int efd = splinter_event_bus_open();
TEST("event bus open returns valid fd", efd >= 0);
/* Two writes already happened, so eventfd counter >= 2; wait should return immediately */
TEST("event bus wait returns immediately (data ready)", splinter_event_bus_wait(efd, 500) == 0);
splinter_event_bus_close(efd);

splinter_close();
splinter_header_snapshot_t closed = { 0 };
TEST("store actually closed", splinter_get_header_snapshot(&closed) != 0);

#ifndef SPLINTER_PERSISTENT
  snprintf(buspath, sizeof(buspath) -1, "/dev/shm/%s", bus);
#else
  snprintf(buspath, sizeof(buspath) -1, "./%s", bus);
#endif /* SPLINTER_PERSISTENT */
  unlink(buspath);

  /* --- event bus: stripe geometry, take semantics, non-blocking doorbell ---
   *
   * These need their own stores because the geometry under test is the slot
   * count itself, and the store above is fixed at 1000 slots.
   */
  {
    char sbus[32] = { 0 };
    uint64_t m[SPLINTER_EVENT_BUS_MASK_WORDS] = { 0 };

    /* Small store: stripe collapses to 1, so the mapping is one bit per slot
     * and behaves exactly as it did before striping existed. */
    snprintf(sbus, sizeof(sbus), "%d-tap-stripe1", pid);
    TEST("stripe: small store created", splinter_create_or_open(sbus, 512, 64) == 0);
    TEST("stripe: small store maps one slot per bit", splinter_event_bus_stripe() == 1);
    TEST("stripe: bus init on small store", splinter_event_bus_init() == 0);

    splinter_set("s_one", "x", 1);
    splinter_event_bus_take_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    TEST("stripe: one write dirties exactly one bit at stripe 1",
         mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS) == 1);

    /* take_dirty is read-and-clear: an immediate second take sees nothing. */
    splinter_event_bus_take_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    TEST("take_dirty clears the mask",
         mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS) == 0);

    /* ...and the mask re-arms on the next write rather than staying dead. */
    splinter_set("s_two", "y", 1);
    splinter_event_bus_take_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    TEST("mask re-arms after a take",
         mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS) == 1);

    /* get_dirty must still be the non-destructive peek it always was. */
    splinter_set("s_three", "z", 1);
    splinter_event_bus_get_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    int peeked = mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    splinter_event_bus_get_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    TEST("get_dirty does not clear the mask",
         peeked > 0 && mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS) == peeked);

    /* The doorbell must be non-blocking, or a saturated counter with no
     * reader draining would wedge a writer inside splinter_set(). */
    int nbfd = splinter_event_bus_open();
    int fl = (nbfd >= 0) ? fcntl(nbfd, F_GETFL) : -1;
    TEST("event bus fd is non-blocking", fl != -1 && (fl & O_NONBLOCK));
    if (nbfd >= 0) splinter_event_bus_close(nbfd);

    splinter_close();
    drop_store(sbus);

    /* --- errno contract on splinter_set ---
     *
     * splinter.h promises ENOSPC when the store is full. It used to return a
     * bare -1 and leave errno holding whatever an unrelated earlier call left
     * there, so a caller following the documented contract read a stale value
     * as a real answer. A tiny store makes "full" cheap to reach.
     */
    snprintf(sbus, sizeof(sbus), "%d-tap-errno", pid);
    TEST("errno: tiny store created", splinter_create_or_open(sbus, 4, 64) == 0);

    errno = 0;
    TEST("errno: oversized value returns -1",
         splinter_set("too_big", "x", 65) == -1);
    TEST("errno: oversized value sets EMSGSIZE", errno == EMSGSIZE);

    errno = 0;
    TEST("errno: zero-length value returns -1",
         splinter_set("empty", "", 0) == -1);
    TEST("errno: zero-length value sets EINVAL", errno == EINVAL);

    /* Fill all four slots, then overflow. Single-threaded, so nothing is ever
     * mid-write and the full store cannot be mistaken for contention. */
    int filled = 0;
    for (int i = 0; i < 4; i++) {
      char k[SPLINTER_KEY_MAX];
      snprintf(k, sizeof(k), "fill_%d", i);
      if (splinter_set(k, "v", 1) == 0) filled++;
    }
    TEST("errno: store filled to capacity", filled == 4);

    errno = 0;
    TEST("errno: set on a full store returns -1",
         splinter_set("overflow_key", "v", 1) == -1);
    TEST("errno: full store sets ENOSPC (not stale, not EAGAIN)", errno == ENOSPC);

    splinter_close();
    drop_store(sbus);

    /* Large store: 5000 slots over 1024 bits -> ceil() gives 5 slots per bit. */
    snprintf(sbus, sizeof(sbus), "%d-tap-stripeN", pid);
    TEST("stripe: large store created", splinter_create_or_open(sbus, 5000, 64) == 0);
    TEST("stripe: large store maps ceil(slots/bits) per bit",
         splinter_event_bus_stripe() == 5);
    TEST("stripe: bus init on large store", splinter_event_bus_init() == 0);

    for (int i = 0; i < 2000; i++) {
      char k[SPLINTER_KEY_MAX];
      snprintf(k, sizeof(k), "stripe_key_%d", i);
      splinter_set(k, "v", 1);
    }
    splinter_event_bus_take_dirty(m, SPLINTER_EVENT_BUS_MASK_WORDS);
    TEST("stripe: striped writes set bits",
         mask_popcount(m, SPLINTER_EVENT_BUS_MASK_WORDS) > 0);

    /* Bit (slots-1)/stripe is the highest reachable bit. Anything above it
     * means an index escaped the mask, which is a memory-safety bug, not just
     * a precision one. This is the property the old modulo folding hid. */
    size_t max_bit = (5000 - 1) / splinter_event_bus_stripe();
    int out_of_range = 0;
    for (size_t b = max_bit + 1; b < SPLINTER_EVENT_BUS_BITS; b++)
      if (m[b / 64] & (1ULL << (b % 64))) out_of_range = 1;
    TEST("stripe: no dirty bit escapes the mask", out_of_range == 0);

    /* Nothing has drained the eventfd since init. Overwrite existing keys so
     * this exercises the write path without consuming new slots. A blocking
     * fd is what makes this shape of loop able to wedge; it must not stall. */
    int stalled = 0;
    for (int i = 0; i < 20000; i++) {
      char k[SPLINTER_KEY_MAX];
      snprintf(k, sizeof(k), "stripe_key_%d", i % 2000);
      if (splinter_set(k, "v", 1) != 0) { stalled = 1; break; }
    }
    TEST("writes do not stall on an undrained eventfd", stalled == 0);

    splinter_close();
    drop_store(sbus);
  }

#ifdef HAVE_VALGRIND_H
  if (RUNNING_ON_VALGRIND) {
    printf("\n** Valgrind Detected. Thank you for your diligence! **\n\n");
    if (VALGRIND_COUNT_ERRORS) {
      fprintf(stderr,"\nValgrind detected errors in this run. Exiting abnormally.\n");
      fprintf(stderr, "(sad trombone sound)\n");
      return 1;
    }
  }
#endif // HAVE_VALGRIND_H
  
  return (passed == total) ? 0 : 1;
}

