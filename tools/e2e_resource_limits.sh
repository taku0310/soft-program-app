#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance check D2: what happens when the host has nothing left to give.
#
# A PLC that half-starts is worse than one that does not start: the process
# image exists, something reads it, and nothing is writing to it. Each case
# below must fail at start-up, exit non-zero, and name the resource it ran
# out of - a supervisor can act on that, and a person can fix it.
#
# The first case is the one that was wrong. ftruncate on tmpfs sets a size
# without reserving pages and mmap does not reserve them either, so on a full
# /dev/shm both succeed and the first write takes SIGBUS: "Bus error", no log
# line, no clue. A container with a small --shm-size is the ordinary way to
# arrive there.
#
# Needs root (mount namespaces) and a built tree:
#   sudo tools/e2e_resource_limits.sh [build-dir]

set -u
B="${1:-./build}"
LOG=$(mktemp -d)
trap 'rm -rf "$LOG"' EXIT

export SOFTPLC_CYCLE_US=10000 SOFTPLC_ROLE=adapter SOFTPLC_MAX_SCANS=200
export SOFTPLC_INSTANCE=d2check

fail=0
check() {
  local name="$1" rc="$2" log="$3" expect="$4"
  if [ "$rc" = 0 ]; then
    echo "  FAIL: $name started anyway (exit 0)"
    fail=1
  elif [ "$rc" -gt 128 ]; then
    echo "  FAIL: $name died on signal $((rc - 128)) instead of reporting"
    fail=1
  elif grep -qi "$expect" "$log"; then
    echo "  PASS: exit $rc, and the message names it:"
    grep -i "$expect" "$log" | head -1 | sed 's/^/        /'
  else
    echo "  FAIL: $name exited $rc but never mentioned '$expect'"
    tail -3 "$log" | sed 's/^/        /'
    fail=1
  fi
}

echo "=== /dev/shm full ==="
unshare -m sh -c "
  mount -t tmpfs -o size=64k tmpfs /dev/shm
  dd if=/dev/zero of=/dev/shm/ballast bs=1k count=64 2>/dev/null
  $B/softplc > $LOG/full.log 2>&1
  echo \$? > $LOG/full.rc
"
check "full /dev/shm" "$(cat "$LOG/full.rc")" "$LOG/full.log" "no space left"

echo "=== no file descriptors left ==="
sh -c "ulimit -n 4; exec $B/softplc" > "$LOG/fds.log" 2>&1
check "fd exhaustion" "$?" "$LOG/fds.log" "too many open files"

echo "=== read-only /dev/shm ==="
unshare -m sh -c "
  mount -t tmpfs -o size=1m,ro tmpfs /dev/shm
  $B/softplc > $LOG/ro.log 2>&1
  echo \$? > $LOG/ro.rc
"
check "read-only /dev/shm" "$(cat "$LOG/ro.rc")" "$LOG/ro.log" "read-only file system"

echo
if [ "$fail" = 0 ]; then
  echo "D2 PASS: every exhausted resource is a named start-up failure"
else
  echo "D2 FAIL"
fi
exit $fail
