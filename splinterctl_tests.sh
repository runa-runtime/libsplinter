#!/bin/sh

# This little scripts tests the splinterctl *interface* to make
# sure we don't break the CLI. It doesn't (and shouldn't) be relied
# on to make sure the underlying library works; that's what the unit
# tests are for. For instance, we don't check to make sure the key
# we get is the key we set *because extensive unit tests already
# did that*, so we're just testing the "workflow" UX.

# Which CLI to exercise. Defaults to the shm-backed splinterctl; override to
# cover the persistent one, which is what ships as splinterpctl:
#   BIN=./splinterpctl ./splinterctl_tests.sh
BIN="${BIN:-./splinterctl}"

TEST_STORE="${$}_splinterctl_test"
ORDERS_STORE="${$}_splinterctl_orders_test"

tests_run=0
tests_passed=0
tests_failed=0

fail()
{
	echo "FAIL: ${@}"
	tests_failed=$(($tests_failed + 1))
	exit 1
}

pass()
{
	tests_passed=$(($tests_passed + 1))
	echo
}

test()
{
	echo "TEST: ${@}"
	tests_run=$(($tests_run + 1))
}

report()
{
	echo "run: ${tests_run} | passed: ${tests_passed} | failed ${tests_failed}"
	[ $tests_run != $tests_passed ] && {
		echo "${tests_failed} tests failed, exiting abnormally."
		# leave the store just in case the CLI introduced a state issue
		exit 1
	}

	echo "Tests passed. Exiting normally."
	# /dev/shm will get very cluttered if you delete the next line:
	rm -f /dev/shm/${TEST_STORE} /dev/shm/${ORDERS_STORE}
	# ...and the persistent CLI writes real files in the cwd instead.
	rm -f ./${TEST_STORE} ./${ORDERS_STORE}
	exit 0
}

test "Initialize store"
"$BIN" init $TEST_STORE || fail "Could not initialize store."
pass

test "Set a key"
"$BIN" --use $TEST_STORE set test_key test_value || fail "Could not set a key."
pass

test "Get a key"
"$BIN" --use $TEST_STORE get test_key || fail "Could not get a key."
pass

test "Get key metadata"
"$BIN" --use $TEST_STORE head test_key || fail "Could not get key metadata."
pass

test "List keys"
"$BIN" --use $TEST_STORE list || fail "Could not list keys"
pass

test "Type name a key"
"$BIN" --use $TEST_STORE type test_key vartext || fail "Could not type test_key as vartext"
pass

test "Get key type"
"$BIN" --use $TEST_STORE type test_key || fail "Could not get type of test_key"
pass

test "Lua script integration"
"$BIN" --use $TEST_STORE lua test.lua || fail "Could not run lua script."
pass

test "Unset a key"
"$BIN" --use $TEST_STORE unset test_key || fail "Could not unset test key"
pass

test "Get global config"
"$BIN" --use $TEST_STORE config || fail "Could not get global config."
pass

test "Set a global config flag"
"$BIN" --use $TEST_STORE config av 1 || fail "Could not set global config flag"
pass

test "Export store metadata"
"$BIN" --use $TEST_STORE export || fail "Could not export JSON"
pass

test "Set a new bump key"
"$BIN" --use $TEST_STORE set bump_key "Bump Value" || fail "Could not set bump key"
pass

test "Bump the bump key"
"$BIN" --use $TEST_STORE bump bump_key || fail "Could not bump the bump key"
pass

test "Append the bump key"
"$BIN"  --use $TEST_STORE append bump_key "\nDo the bumpty-bump." || fail "Could not append the bump key"
pass

test "Generate a UUID"
"$BIN" uuid || fail "Did not generate a UUID"
pass

# --- regressions -----------------------------------------------------------
# Both of these guard bugs that were silent in the field: the CLI reported
# success while doing the wrong thing. They assert exit codes, not output.

test "Reject an over-long store path instead of silently truncating it"
LONG_STORE=$(awk 'BEGIN { while (i++ < 4200) printf "z" }')
# A store path used to be copied into a fixed 64-byte buffer, so anything
# longer was cut down to 63 bytes and created under THAT name -- a different,
# perfectly valid path -- with no warning and a zero exit.
"$BIN" init "$LONG_STORE" >/dev/null 2>&1 && \
	fail "init accepted a store path longer than PATH_MAX"
TRUNCATED=$(printf '%.63s' "$LONG_STORE")
[ -e "$TRUNCATED" ] || [ -e "/dev/shm/${TRUNCATED}" ] && \
	fail "init created a store under a truncated name: ${TRUNCATED}"
pass

test "orders set reports failure through its exit code, not just stdout"
"$BIN" init $ORDERS_STORE --slots 4 --length 1024 >/dev/null 2>&1 || \
	fail "Could not initialize the orders test store."
# Each tandem costs two slots, so a 4-slot store holds exactly two of them.
"$BIN" --use $ORDERS_STORE set order_a "-" >/dev/null 2>&1 || fail "Could not set order_a"
"$BIN" --use $ORDERS_STORE orders set order_a 2 >/dev/null 2>&1 || \
	fail "First tandem should have succeeded"
"$BIN" --use $ORDERS_STORE set order_b "-" >/dev/null 2>&1 || fail "Could not set order_b"
"$BIN" --use $ORDERS_STORE orders set order_b 2 >/dev/null 2>&1 || \
	fail "Second tandem should have succeeded"
# The store is full now. This used to print "... FAIL" and still exit 0.
"$BIN" --use $ORDERS_STORE set order_c "-" >/dev/null 2>&1
"$BIN" --use $ORDERS_STORE orders set order_c 2 >/dev/null 2>&1 && \
	fail "orders set exited 0 after failing to set the tandem"
pass

report
