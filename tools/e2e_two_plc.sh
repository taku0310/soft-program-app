#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Two soft PLCs talking real EtherNet/IP to each other: one in the Adapter
# (CIP target) role, one in the Scanner (originator) role, each in its own
# network namespace.
#
# The namespaces are not a convenience. CIP class 1 uses a fixed UDP port
# (2222) at both ends, so two EtherNet/IP endpoints on one host fight over it
# and each receives its own transmissions - the symptom is EIPScanner logging
# "Received data from unknown connection T2O_ID=<our own O2T id>". Separate
# namespaces are the local stand-in for the two machines a real line has.
#
# Requires root (netns) and iproute2. Build first with both stacks:
#   cmake -S . -B build-scan -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON
#   cmake --build build-scan -j
#
# Then:  sudo tools/e2e_two_plc.sh [build-dir] [work-dir]
#
# Each side sends a distinct pattern and reports what arrives. "A sees B's
# pattern and B sees A's" cannot be faked by a loopback or a stuck buffer.

set -u
B="${1:-./build-scan}"
S="${2:-/tmp/softplc-e2e}"
mkdir -p "$S"

# --- namespaces: two "machines" joined by a veth pair ---------------------
setup_netns() {
  ip netns del plcA 2>/dev/null; ip netns del plcB 2>/dev/null
  ip link del vethA 2>/dev/null
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
}
setup_netns || { echo "netns setup failed (need root + iproute2)" >&2; exit 1; }

# Clean slate: kill by binary path, not a pattern that would match this script.
for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc softplc; do
  for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
done
sleep 1
rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.* "$S"/e2e*.log

# RPI is a knob because it interacts with the scan rate: the process image
# holds the last frame received, so a scan at or faster than the RPI re-reads
# the same value. E2E_RPI_US lets that be demonstrated rather than asserted.
RPI_US="${E2E_RPI_US:-10000}"
cat > "$S/devA.conf" <<EOF
10.10.0.1  151 150 100  32 32  $RPI_US ${E2E_TMO_MULT:-2} hold
EOF

# PLC A runs longer than B on purpose: whichever side exits first unlinks its
# IPC objects and takes its stack down, which would drop the other's CIP
# connection mid-measurement.
# --- PLC A (10.10.0.1): Adapter role, sends 0xA1 ---
ip netns exec plcA env SOFTPLC_LOG_LEVEL=info \
  $B/e2e_two_plc adapter plcA 0xA1 1200 > "$S/e2eA.log" 2>&1 &
PLC_A=$!
sleep 1
ip netns exec plcA $B/softplc-eip-adapter plcA vethA > "$S/e2eA-eip.log" 2>&1 &
sleep 3

# The mirror backends answer the same interfaces as the real stacks, so a tree
# built without them runs this whole script and reports ONLINE with corrupt=0
# having sent no CIP at all: the loopback mirrors each side's outputs back to
# its own inputs, and only peer_frames=0 gives it away. Measured on such a
# build - exit 0, both sides ONLINE, zero ForwardOpens. Refuse it instead.
if grep -q "backend 'loopback'" "$S/e2eA-eip.log" 2>/dev/null; then
    echo "REFUSING: $B was built without OpENer, so the adapter is a mirror" >&2
    echo "  cmake -S . -B $B -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON" >&2
    exit 2
fi

# --- PLC B (10.10.0.2): Scanner role, sends 0xB2 ---
ip netns exec plcB env SOFTPLC_LOG_LEVEL=info SOFTPLC_SCANNER_DEVICES="$S/devA.conf" \
  $B/e2e_two_plc scanner plcB 0xB2 500 > "$S/e2eB.log" 2>&1 &
PLC_B=$!
sleep 1
ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
  $B/softplc-eip-scanner plcB "$S/devA.conf" > "$S/e2eB-eip.log" 2>&1 &

# --- mid-run: drop and restore PLC A's CIP stack -------------------------
# Exercises the paths that only a real connection loss reaches: OpENer's
# CheckIoConnectionEvent on the target side, and on the scanner side the
# close listener plus the gated reconnect. Nothing else in the suite runs
# them, because the mirror backends never lose a connection.
if [ "${E2E_RECONNECT:-1}" = "1" ]; then
  sleep 4
  echo "--- dropping PLC A's CIP stack ---"
  for pid in $(pgrep -f "$B/softplc-eip-adapter" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  sleep 3
  echo "--- restoring it ---"
  ip netns exec plcA $B/softplc-eip-adapter plcA vethA >> "$S/e2eA-eip.log" 2>&1 &
fi

# Only the two PLCs finish on their own; the stack processes serve until they
# are stopped, so waiting on them would hang here forever with the measurement
# already complete.
wait "$PLC_A"; RC_A=$?
wait "$PLC_B"; RC_B=$?
for exe in softplc-eip-adapter softplc-eip-scanner; do
  for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
done

echo
echo "================ RESULT ================"
grep -hE "tag=|exchanges=" "$S/e2eA.log" "$S/e2eB.log" 2>/dev/null
grep -h "device health" "$S/e2eB.log" 2>/dev/null
echo
echo "Pass criteria:"
echo "  peer_tag is the OTHER side's tag (A=0xA1, B=0xB2) - not a loopback"
echo "  fresh > 0                                          - frames keep changing"
echo "  corrupt = 0                                        - byte offsets intact"
echo "  the scanner reconnected after the stack was killed mid-run"
echo
echo "Reconnect evidence (scanner):"
FOPENS=$(grep -c "Open IO connection" "$S/e2eB-eip.log" 2>/dev/null)
echo "  ForwardOpen count (>1 means it reconnected): $FOPENS"
echo "Logs: $S/e2e*.log"

# Each side already decides pass or fail against the criteria above and says so
# on its stderr, which lands in its log. Discarding both and exiting 0 made
# every run look like a success, including the ones that carried no traffic.
echo
FAIL=0
if grep -q "backend 'mirror'" "$S/e2eB-eip.log" 2>/dev/null; then
    echo "FAIL: $B was built without EIPScanner - the scanner was a mirror, so"
    echo "      nothing here crossed a wire. Rebuild with:"
    echo "      cmake -S . -B $B -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON"
    FAIL=1
fi
[ "$RC_A" = 0 ] || { echo "FAIL: PLC A (adapter) - $(grep -h 'FAIL' "$S/e2eA.log" | tail -1)"; FAIL=1; }
[ "$RC_B" = 0 ] || { echo "FAIL: PLC B (scanner) - $(grep -h 'FAIL' "$S/e2eB.log" | tail -1)"; FAIL=1; }
if [ "${E2E_RECONNECT:-1}" = "1" ] && [ "${FOPENS:-0}" -lt 2 ]; then
    echo "FAIL: the scanner did not reconnect after the stack was killed"
    FAIL=1
fi
if [ "$FAIL" = 0 ]; then
    echo "PASS: two soft PLCs exchanged real CIP, and the link survived a stack restart"
fi
exit $FAIL
