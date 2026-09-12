#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance check B4: malformed input on the class 1 I/O port.
#
# Starts a real Scanner against a real Adapter, then sends deliberately
# malformed datagrams at the scanner's UDP 2222 from a third namespace - an
# attacker, a broken device, or a stray broadcast. The requirement is that
# each one costs a dropped frame: both stacks alive at the end, and the
# connection carrying data again.
#
# This found the worst defect in the acceptance set. Upstream's Buffer copied
# a length taken from the datagram without checking it against what had
# arrived, so one item claiming more bytes than were present read off the end
# of the heap: SIGSEGV, from a single unauthenticated UDP packet. Fixed in
# patches/eipscanner-bounds.patch.
#
# Requires root and a build with both stacks:
#   sudo tools/e2e_cip_fuzz.sh [build-dir] [work-dir] [count]

set -u
B="${1:-./build-scan}"
W="${2:-/tmp/softplc-fuzz}"
COUNT="${3:-400}"
mkdir -p "$W"

cleanup() {
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc cip_fuzz; do
    for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  done
  for ns in plcA plcB evil; do
    ip netns del "$ns" 2>/dev/null
    # Deleting the namespace takes its end of the pair with it, but not
    # instantly, and the bridge-side name is what the next case re-creates:
    # a leftover turns setup into "RTNETLINK answers: File exists" and the
    # case runs against half a topology.
    ip link del "veth-$ns" 2>/dev/null
  done
  ip link del br0 2>/dev/null
  # Let the kernel finish tearing them down before anything is re-added.
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    ip link show veth-plcA >/dev/null 2>&1 || break
    sleep 0.2
  done
}
trap cleanup EXIT
cleanup

# A bridge, so a third party can reach the scanner the way anything on a plant
# network can. The impairment tests need a wire in the middle; this one needs
# a neighbour.
ip netns add plcA && ip netns add plcB && ip netns add evil || exit 1
ip link add br0 type bridge
ip link set br0 up
for ns in plcA plcB evil; do
  ip link add "veth-$ns" type veth peer name "in-$ns"
  ip link set "veth-$ns" master br0
  ip link set "veth-$ns" up
  ip link set "in-$ns" netns "$ns"
  ip netns exec "$ns" ip link set lo up
  ip netns exec "$ns" ip link set "in-$ns" up
done
ip netns exec plcA ip addr add 10.10.0.1/24 dev in-plcA
ip netns exec plcB ip addr add 10.10.0.2/24 dev in-plcB
ip netns exec evil ip addr add 10.10.0.3/24 dev in-evil
rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*

printf '10.10.0.1 151 150 100 32 32 10000 2 hold\n' > "$W/dev.conf"

ip netns exec plcA $B/e2e_two_plc adapter plcA 0xA1 4000 > "$W/A.log" 2>&1 &
sleep 1
ip netns exec plcA $B/softplc-eip-adapter plcA in-plcA > "$W/A-eip.log" 2>&1 &
ADAPTER=$!
sleep 2
ip netns exec plcB env SOFTPLC_SCANNER_DEVICES="$W/dev.conf" \
  $B/e2e_two_plc scanner plcB 0xB2 2000 > "$W/B.log" 2>&1 &
VERIFIER=$!
sleep 1
ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
  $B/softplc-eip-scanner plcB "$W/dev.conf" > "$W/B-eip.log" 2>&1 &
SCANNER=$!
sleep 4

echo "--- fuzzing the scanner's I/O port from a third host ---"
ip netns exec evil $B/cip_fuzz 10.10.0.2 "$COUNT" 7 2>&1 | tail -1
echo "--- and the adapter's ---"
ip netns exec evil $B/cip_fuzz 10.10.0.1 "$COUNT" 7 2>&1 | tail -1

wait $VERIFIER 2>/dev/null
rc=$?

fail=0
kill -0 $SCANNER 2>/dev/null && echo "scanner stack: ALIVE" || { echo "scanner stack: DEAD"; fail=1; }
kill -0 $ADAPTER 2>/dev/null && echo "adapter stack: ALIVE" || { echo "adapter stack: DEAD"; fail=1; }
grep -hE "peer_tag=" "$W/B.log" | sed 's/^/  /'
# Piping grep into sed hides grep's status - sed always succeeds - so the
# match has to be tested on its own.
if grep -qiE "terminate called|segmentation|core dumped" "$W/A-eip.log" "$W/B-eip.log"; then
  grep -hiE "terminate called|segmentation|core dumped" "$W/A-eip.log" "$W/B-eip.log" \
    | sed 's/^/  CRASH: /'
  fail=1
fi
[ "$rc" = 0 ] || fail=1

echo
if [ "$fail" = 0 ]; then
  echo "B4 PASS: both stacks survived $((COUNT * 2)) malformed datagrams and kept serving"
else
  echo "B4 FAIL: see $W/*.log"
fi
exit $fail
