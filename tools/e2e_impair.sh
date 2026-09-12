#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance checks B1c, B3 and B4: what survives a damaged wire.
#
# netem is not available on this kernel (CONFIG_NET_SCH_NETEM unset), and
# iptables can only drop - it cannot hold a frame back, which is the mechanism
# behind every reordering test. So the wire is a process: three namespaces,
# with `cip_impair` forwarding frames between the two halves of the link and
# damaging them on the way through.
#
#   plcA (10.10.0.1) --vethA=vethMA-- [mid: cip_impair] --vethMB=vethB-- plcB (10.10.0.2)
#
# Only CIP class 1 I/O (UDP 2222) is damaged. ARP and TCP 44818 cross intact,
# so these tests impair the I/O path while leaving session management alone -
# the distinction that separates them from B2.
#
# Requires root and a build with both stacks:
#   sudo tools/e2e_impair.sh <case> [build-dir] [work-dir] [seconds]
#
#   baseline   the wire with nothing turned on - proves the forwarder itself
#              is not what a later case is measuring
#   reorder    B1c: 10% of frames held back by two RPIs, so later ones pass first
#   jitter     B1c: 2 ms delay with 8 ms of jitter, which reorders by itself
#   replay     B3: duplicates plus frames replayed from 8 back
#   malformed  B4: truncated and bit-flipped frames
#   all        every case in turn
#
# Pass criteria are per case and printed with the result.

set -u
CASE="${1:-all}"
B="${2:-./build-scan}"
W="${3:-/tmp/softplc-impair}"
SECS="${4:-25}"
mkdir -p "$W"

cleanup() {
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc cip_impair; do
    for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  done
  ip netns del plcA 2>/dev/null; ip netns del plcB 2>/dev/null; ip netns del mid 2>/dev/null
  ip link del vethA 2>/dev/null; ip link del vethB 2>/dev/null
}
trap cleanup EXIT

# Two veth pairs and a namespace in the middle holding one end of each. The
# middle has no addresses: it is a wire, not a router, and forwards at L2.
setup() {
  cleanup
  ip netns add plcA && ip netns add plcB && ip netns add mid || return 1
  ip link add vethA type veth peer name vethMA || return 1
  ip link add vethB type veth peer name vethMB || return 1
  ip link set vethA  netns plcA
  ip link set vethMA netns mid
  ip link set vethB  netns plcB
  ip link set vethMB netns mid
  ip netns exec plcA ip addr add 10.10.0.1/24 dev vethA
  ip netns exec plcA ip link set vethA up; ip netns exec plcA ip link set lo up
  ip netns exec plcB ip addr add 10.10.0.2/24 dev vethB
  ip netns exec plcB ip link set vethB up; ip netns exec plcB ip link set lo up
  ip netns exec mid ip link set vethMA up
  ip netns exec mid ip link set vethMB up
  ip netns exec mid ip link set lo up
  rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*
}

run_case() {
  local name="$1"; shift
  local log="$W/$name"
  mkdir -p "$log"
  setup || return 1

  printf '10.10.0.1 151 150 100 32 32 10000 2 hold\n' > "$log/dev.conf"

  # The wire first: it must be forwarding before either stack tries to ARP.
  # The wire has to outlive both PLCs, including the tail where A is still
  # running after B has finished.
  ip netns exec mid $B/cip_impair vethMA vethMB "$(( SECS + 12 ))" "$@" \
    > "$log/impair.log" 2>&1 &
  local wire=$!
  sleep 1

  local scans=$(( (SECS - 6) * 100 ))
  # A outlives B by a few seconds - whichever side exits first takes its stack
  # down and drops the other's connection mid-measurement - but it must still
  # end on its own, or the adapter direction never prints what it saw. Both
  # directions are impaired, so both have to be read.
  ip netns exec plcA $B/e2e_two_plc adapter plcA 0xA1 $(( scans + 500 )) > "$log/A.log" 2>&1 &
  local plcA_pid=$!
  sleep 1
  ip netns exec plcA $B/softplc-eip-adapter plcA vethA > "$log/A-eip.log" 2>&1 &
  sleep 2
  # The core needs the device table too, not just the stack: without it the
  # scanner proxy has nothing to lay out and exits before the wire sees a frame.
  ip netns exec plcB env SOFTPLC_SCANNER_DEVICES="$log/dev.conf" \
    $B/e2e_two_plc scanner plcB 0xB2 "$scans" > "$log/B.log" 2>&1 &
  local verifier=$!
  sleep 1
  ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
    $B/softplc-eip-scanner plcB "$log/dev.conf" > "$log/B-eip.log" 2>&1 &

  wait $verifier 2>/dev/null
  local rc=$?
  # A finishes a few seconds later by design; its summary is the only report
  # of the adapter direction, so it is waited for rather than cleaned up.
  wait $plcA_pid 2>/dev/null
  kill -TERM $wire 2>/dev/null; wait $wire 2>/dev/null

  echo "  wire:"
  sed 's/^/    /' "$log/impair.log"
  echo "  scanner (reads T->O, impaired b->a):"
  grep -hE "peer_tag=" "$log/B.log" | sed 's/^/    /'
  echo "  adapter (reads O->T, impaired a->b):"
  grep -hE "peer_tag=" "$log/A.log" | sed 's/^/    /'
  echo "  connection churn: ForwardOpens=$(grep -c 'Open IO connection' "$log/B-eip.log" 2>/dev/null)" \
       "closes=$(grep -c 'is closed by timeout' "$log/B-eip.log" 2>/dev/null)"
  # A stack that died is the headline finding of B4, so look for it explicitly
  # rather than inferring it from a missing number.
  for side in A-eip B-eip; do
    pgrep -f "$B/softplc-eip" >/dev/null || true
  done
  grep -hiE "signal|segmentation|abort|terminate|assert|what\(\)" \
       "$log/A-eip.log" "$log/B-eip.log" 2>/dev/null | sed 's/^/    CRASH? /'
  return $rc
}

case "$CASE" in
  baseline)  echo "=== baseline: the wire, undamaged ==="
             run_case baseline ;;
  reorder)   echo "=== B1c reorder: 10% held back by 20 ms (two RPIs) ==="
             run_case reorder reorder_pct=10 reorder_gap_us=20000 seed=1 ;;
  jitter)    echo "=== B1c jitter: 2 ms delay, 8 ms jitter ==="
             run_case jitter delay_us=2000 jitter_us=8000 seed=2 ;;
  replay)    echo "=== B3 replay: 10% duplicated, 10% replayed from 8 frames back ==="
             run_case replay dup_pct=10 replay_pct=10 replay_age=8 seed=3 ;;
  malformed) echo "=== B4 malformed: 5% truncated, 5% bit-flipped ==="
             run_case malformed trunc_pct=5 corrupt_pct=5 seed=4 ;;
  all)
    rc=0
    for c in baseline reorder jitter replay malformed; do
      "$0" "$c" "$B" "$W" "$SECS" || rc=1
      echo
    done
    exit $rc ;;
  *) echo "unknown case: $CASE" >&2; exit 2 ;;
esac
