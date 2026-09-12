#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance checks C3 and D3: core and stack at different versions.
#
# They are separate binaries in separate containers, so a rolling update runs
# them at different versions for a while. The shared region carries an ABI
# version and its own size for exactly that window. What must not happen is
# the newer stack attaching anyway and reading fields at offsets that have
# moved: a shifted process image drives machinery with the wrong numbers, and
# nothing above it can tell.
#
# The skewed stack is a real second build of the tree with the ABI constant
# bumped - not a simulation - because the thing under test is what two
# independently built binaries do when they meet.
#
#   sudo tools/e2e_version_skew.sh <current-build> <skewed-build>
#
# Build the skewed side with:
#   cp -r . /tmp/skew && sed -i 's/#define EIP_SHM_ABI_VERSION .*/#define EIP_SHM_ABI_VERSION 99u/' \
#       /tmp/skew/src/adapters/eip/eip_shm_layout.h
#   cmake -S /tmp/skew -B /tmp/skew-build -DSOFTPLC_WITH_OPENER=ON
#   cmake --build /tmp/skew-build --target softplc-eip-adapter -j
#
# Three things are asserted, in order: the mismatched stack refuses and says
# why; the core it refused keeps scanning and holds a failsafe image rather
# than serving whatever is in memory; and the matching stack, started after,
# brings it back online. The last is what makes it a rolling-update test
# rather than a version check.

set -u
CUR="${1:-./build}"
SKEW="${2:-/tmp/skew-build}"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$W"' EXIT

[ -x "$SKEW/softplc-eip-adapter" ] || {
  echo "no skewed adapter at $SKEW/softplc-eip-adapter - see the header of this script" >&2
  exit 2
}

rm -f /dev/shm/softplc.skew* /dev/shm/sem.softplc.skew*
export SOFTPLC_INSTANCE=skew SOFTPLC_ROLE=adapter SOFTPLC_CYCLE_US=10000
export SOFTPLC_MAX_SCANS=1200 SOFTPLC_ADAPTERS=ethernet-ip

fail=0

"$CUR/softplc" > "$W/core.log" 2>&1 &
CORE=$!
sleep 2

echo "=== 1. the stack from the other version attaches ==="
"$SKEW/softplc-eip-adapter" skew lo > "$W/skewed.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then
  echo "  FAIL: it attached and served (exit 0)"; fail=1
elif grep -qiE "abi|layout|magic" "$W/skewed.log"; then
  echo "  PASS: refused (exit $rc), naming the mismatch:"
  grep -iE "abi|layout|magic" "$W/skewed.log" | head -1 | sed 's/^/        /'
else
  echo "  FAIL: exited $rc but never said why"; tail -3 "$W/skewed.log" | sed 's/^/        /'; fail=1
fi

echo "=== 2. the core it refused ==="
sleep 2
if "$CUR/softplc" --status skew > "$W/status1.json" 2>&1; then
  echo "  FAIL: reported ready with no stack attached"; fail=1
else
  echo "  PASS: alive and not ready ($(sed 's/.*"state":"\([A-Z]*\)".*/\1/' "$W/status1.json"))"
fi
kill -0 $CORE 2>/dev/null && echo "  PASS: the core is still scanning" \
  || { echo "  FAIL: the core died"; fail=1; }
grep -qi "failsafe" "$W/core.log" \
  && echo "  PASS: it applied the failsafe rather than serving stale memory" \
  || { echo "  FAIL: no failsafe in the core log"; fail=1; }

echo "=== 3. the matching stack, started after ==="
"$CUR/softplc-eip-adapter" skew lo > "$W/matched.log" 2>&1 &
MATCHED=$!
sleep 3
if grep -qi "attached to" "$W/matched.log"; then
  echo "  PASS: attached:"
  grep -i "attached to" "$W/matched.log" | head -1 | sed 's/^/        /'
else
  echo "  FAIL: the matching stack did not attach"; tail -3 "$W/matched.log" | sed 's/^/        /'; fail=1
fi
"$CUR/softplc" --status skew > "$W/status2.json" 2>&1
grep -q '"state":"ONLINE"' "$W/status2.json" \
  && echo "  PASS: back ONLINE after the update completed" \
  || { echo "  FAIL: still not online"; cat "$W/status2.json" | sed 's/^/        /'; fail=1; }

kill -9 $MATCHED $CORE 2>/dev/null
wait 2>/dev/null

echo
if [ "$fail" = 0 ]; then
  echo "C3/D3 PASS: a skewed stack is refused, the core stays safe, and the update completes"
else
  echo "C3/D3 FAIL"
fi
exit $fail
