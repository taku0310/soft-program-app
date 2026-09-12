#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance check B2: a cable pull, not a dropped port.
#
# B1b's outage was `iptables -A OUTPUT -p udp --dport 2222 -j DROP`. That
# stops the class 1 I/O and nothing else: the TCP session on 44818 stays
# established, its socket stays open, and recovery is one ForwardOpen. Pulling
# a cable is a different fault. The TCP session dies with the link, so coming
# back needs the session registered again and a new ForwardOpen on top - the
# path that had never once executed here, and A4 found a defect in the half of
# it iptables does reach.
#
# `ip link set <veth> down` in the adapter's namespace is that fault: carrier
# gone, both protocols, both directions, no filtering anywhere.
#
# Requires root (netns) and a build with both stacks:
#   cmake -S . -B build-scan -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON
#   cmake --build build-scan -j
#
# Then:  sudo tools/e2e_link_down.sh [build-dir] [work-dir] [down-seconds]
#
# Runs hold and clear. Both must report PASS: outage seen, policy held while
# it lasted, link re-established, and the image moving again afterwards.

set -u
B="${1:-./build-scan}"
W="${2:-/tmp/softplc-linkdown}"
DOWN_S="${3:-6}"
mkdir -p "$W"

cleanup() {
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_link_survive e2e_two_plc; do
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

  ip netns exec plcA $B/e2e_two_plc adapter plcA 0xA1 100000 > "$log/A.log" 2>&1 &
  sleep 1
  ip netns exec plcA $B/softplc-eip-adapter plcA vethA > "$log/A-eip.log" 2>&1 &
  sleep 2

  # 2200 scans at 10 ms is 22 s: 5 s up, the outage, and enough time after it
  # for the reconnect plus the post-recovery freshness window.
  ip netns exec plcB $B/e2e_link_survive plcB "$log/dev.conf" 2200 "$policy" \
    > "$log/verdict.log" 2>&1 &
  local verifier=$!
  sleep 1
  ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
    $B/softplc-eip-scanner plcB "$log/dev.conf" > "$log/B-eip.log" 2>&1 &

  sleep 5
  echo "  --- link down (carrier, TCP and UDP together) ---"
  ip netns exec plcA ip link set vethA down
  local t_down; t_down=$(date +%s%6N)
  sleep "$DOWN_S"
  echo "  --- link up ---"
  ip netns exec plcA ip link set vethA up
  ip netns exec plcA ip addr add 10.10.0.1/24 dev vethA 2>/dev/null
  local t_up; t_up=$(date +%s%6N)

  wait $verifier
  local rc=$?
  grep -h "^linksurv:" "$log/verdict.log"

  # Detection and recovery latency. The verifier stamps each transition with
  # wall time because only this function knows when the cable moved.
  local w_down w_up
  w_down=$(sed -n 's/.*outage visible.*at_wall_us=\([0-9]*\).*/\1/p' "$log/verdict.log")
  w_up=$(sed -n 's/.*back online.*at_wall_us=\([0-9]*\).*/\1/p' "$log/verdict.log")
  [ -n "$w_down" ] && echo "  carrier loss -> image shows it : $(( (w_down - t_down) / 1000 )) ms"
  [ -n "$w_up" ]   && echo "  carrier back -> device online  : $(( (w_up - t_up) / 1000 )) ms"

  # What the scanner had to do to get back. "Unregistered session" also
  # contains "egistered session", so the count has to exclude it: the claim
  # is a *new* session, and a sloppy grep would report one that never
  # happened.
  printf '  session registrations: %s, session losses: %s, ForwardOpens: %s\n' \
    "$(grep -c '^\[INFO\] Registered session' "$log/B-eip.log" 2>/dev/null)" \
    "$(grep -c 'session to .* failed' "$log/B-eip.log" 2>/dev/null)" \
    "$(grep -c 'Open IO connection' "$log/B-eip.log" 2>/dev/null)"
  return $rc
}

fail=0
for policy in hold clear; do
  echo "=== $policy ==="
  run_policy "$policy" || fail=1
done

echo
if [ "$fail" = 0 ]; then
  echo "B2 PASS: the link came back on its own under both policies"
else
  echo "B2 FAIL: see $W/*/verdict.log and $W/*/B-eip.log"
fi
exit $fail
