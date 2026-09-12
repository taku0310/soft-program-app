#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance check A4: does the failsafe hold the right *bytes* when a real
# CIP connection drops under a peer that is still alive?
#
# The unit suite covers HOLD and CLEAR by killing the stack process, which
# exercises the IPC boundary. This covers the other side of it: the scanner
# process keeps answering, the Adapter keeps running, and only the class 1
# connection between them goes away. What a POU reads in that state is what
# this asserts, on every scan the link is down - not just the first.
#
# The outage is an iptables DROP on UDP 2222 in both namespaces, held to the
# end of the run. Dropping only the I/O port leaves TCP 44818 up, so the
# scanner can still complete a ForwardOpen while no data flows - which is
# precisely the state that used to be reported as ONLINE.
#
# Requires root (netns, iptables) and a build with both stacks:
#   cmake -S . -B build-scan -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON
#   cmake --build build-scan -j
#
# Then:  sudo tools/e2e_cip_failsafe.sh [build-dir] [work-dir]
#
# Runs once with a device table set to hold and once set to clear; both must
# report PASS.

set -u
B="${1:-./build-scan}"
W="${2:-/tmp/softplc-cipfs}"
mkdir -p "$W"

cleanup() {
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_cip_failsafe e2e_two_plc; do
    for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  done
  ip netns del plcA 2>/dev/null; ip netns del plcB 2>/dev/null
  ip link del vethA 2>/dev/null
}
trap cleanup EXIT

run_policy() {
  local policy="$1"
  local log="$W/$policy"
  mkdir -p "$log"

  cleanup
  ip netns add plcA && ip netns add plcB || return 1
  ip link add vethA type veth peer name vethB || return 1
  ip link set vethA netns plcA
  ip link set vethB netns plcB
  ip netns exec plcA ip addr add 10.10.0.1/24 dev vethA
  ip netns exec plcA ip link set vethA up
  ip netns exec plcA ip link set lo up
  ip netns exec plcB ip addr add 10.10.0.2/24 dev vethB
  ip netns exec plcB ip link set vethB up
  ip netns exec plcB ip link set lo up
  rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*

  printf '10.10.0.1 151 150 100 32 32 10000 2 %s\n' "$policy" > "$log/dev.conf"

  # PLC A: Adapter role, producing a pattern that changes every scan, so a
  # held image is distinguishable from a live one.
  ip netns exec plcA $B/e2e_two_plc adapter plcA 0xA1 100000 > "$log/A.log" 2>&1 &
  sleep 1
  ip netns exec plcA $B/softplc-eip-adapter plcA vethA > "$log/A-eip.log" 2>&1 &
  sleep 2

  # PLC B: the verifier, in the Scanner role.
  ip netns exec plcB $B/e2e_cip_failsafe plcB "$log/dev.conf" 900 "$policy" norecover \
    > "$log/verdict.log" 2>&1 &
  local verifier=$!
  sleep 1
  ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=warning \
    $B/softplc-eip-scanner plcB "$log/dev.conf" > "$log/B-eip.log" 2>&1 &

  sleep 5
  # The connection loss: I/O stops, both peers stay up. Longer than the
  # (4 << 2) x 10 ms = 160 ms connection budget, so it genuinely times out.
  ip netns exec plcA iptables -A OUTPUT -p udp --dport 2222 -j DROP
  ip netns exec plcB iptables -A OUTPUT -p udp --dport 2222 -j DROP

  wait $verifier
  local rc=$?
  grep -h "^cipfs:" "$log/verdict.log"
  return $rc
}

fail=0
for policy in hold clear; do
  echo "=== $policy ==="
  run_policy "$policy" || fail=1
done

echo
if [ "$fail" = 0 ]; then
  echo "A4 PASS: both policies reproduced the right image across a real CIP loss"
else
  echo "A4 FAIL: see $W/*/verdict.log"
fi
exit $fail
