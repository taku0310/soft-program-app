#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Verifies the claim the single-container deployment rests on: killing the
# protocol stack does not take the PLC down.
#
# It SIGKILLs the real stack process mid-run and then asserts three things -
# the core's pid never changed, the stack came back, and the core's own
# shutdown line accounts for a run that continued across the gap.  A mocked
# stack could not demonstrate any of that, which is why this kills a real one.
#
#   tools/entrypoint_restart_check.sh [build-dir]

set -uo pipefail

BUILD=${1:-build}
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOG=$(mktemp)
INSTANCE="epchk-$$"

cleanup() {
    [[ -n ${EP_PID:-} ]] && kill -TERM "$EP_PID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; echo "--- log ---" >&2; cat "$LOG" >&2; exit 1; }

# Wait for line N of a pattern to appear, or give up.  Polling a log is
# crude, but the alternative is a fixed sleep, which is the thing that makes a
# timing test flaky rather than the thing that makes it robust.
wait_for() {
    local pattern=$1 count=$2 timeout=${3:-15} waited=0
    while (( waited < timeout * 10 )); do
        [[ $(grep -c "$pattern" "$LOG") -ge $count ]] && return 0
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Refuse a stale build tree rather than exercising one.  An older `softplc`
# does not know `--role` and treats it as no argument at all, so it starts
# scanning instead of answering - which looks like an entrypoint bug six
# seconds later, and is not one.
# An absolute build directory is not a subdirectory of the repository, and
# gluing $HERE onto one produces a path that exists nowhere.
case "$BUILD" in
    /*) BIN="$BUILD" ;;
    *)  BIN="$HERE/$BUILD" ;;
esac
[[ -x "$BIN/softplc" ]] || fail "no softplc binary in $BIN"
"$BIN/softplc" --list-roles 2>/dev/null | grep -q '^  adapter' \
    || fail "$BIN/softplc has no 'adapter' role - stale build directory?"

SOFTPLC_BIN_DIR="$BIN" \
SOFTPLC_ROLE=adapter \
SOFTPLC_INSTANCE="$INSTANCE" \
SOFTPLC_CYCLE_US=2000 \
SOFTPLC_MAX_SCANS=3000 \
SOFTPLC_STACK_RESTART_DELAY_MS=200 \
    bash "$HERE/docker/entrypoint.sh" >"$LOG" 2>&1 &
EP_PID=$!

wait_for 'started softplc-eip-adapter' 1 || fail "the stack never started"
wait_for 'started softplc as pid'      1 || fail "the core never started"

core_pid=$(sed -n 's/.*started softplc as pid \([0-9]*\).*/\1/p' "$LOG" | head -1)
stack_pid=$(sed -n 's/.*started softplc-eip-adapter as pid \([0-9]*\).*/\1/p' "$LOG" | head -1)
echo "core=$core_pid stack=$stack_pid; killing the stack"

kill -9 "$stack_pid" || fail "could not kill the stack process $stack_pid"

wait_for 'the protocol stack exited' 1 || fail "the death was never noticed"
wait_for 'started softplc-eip-adapter' 2 || fail "the stack was never restarted"

# The point of the whole exercise: the PLC is the same process it was before.
kill -0 "$core_pid" 2>/dev/null || fail "the core died with the stack"

wait -n
rc=$?
(( rc == 0 )) || fail "the entrypoint exited $rc"

grep -q "applying HOLD failsafe" "$LOG" \
    || fail "the core never applied its failsafe across the gap"
scans=$(sed -n 's/.*stopped after \([0-9]*\) scans.*/\1/p' "$LOG")
[[ ${scans:-0} -eq 3000 ]] \
    || fail "the core ran $scans scans, not the 3000 it was asked for"

echo "PASS: core pid $core_pid survived the stack being killed; $scans scans, stack restarted"
