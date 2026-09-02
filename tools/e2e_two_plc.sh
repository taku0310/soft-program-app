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

cat > "$S/devA.conf" <<EOF
10.10.0.1  151 150 100  32 32  10000 hold
EOF

# PLC A runs longer than B on purpose: whichever side exits first unlinks its
# IPC objects and takes its stack down, which would drop the other's CIP
# connection mid-measurement.
# --- PLC A (10.10.0.1): Adapter role, sends 0xA1 ---
ip netns exec plcA env SOFTPLC_LOG_LEVEL=info \
  $B/e2e_two_plc adapter plcA 0xA1 1200 > "$S/e2eA.log" 2>&1 &
sleep 1
ip netns exec plcA $B/softplc-eip-adapter plcA vethA > "$S/e2eA-eip.log" 2>&1 &
sleep 3

# --- PLC B (10.10.0.2): Scanner role, sends 0xB2 ---
ip netns exec plcB env SOFTPLC_LOG_LEVEL=info SOFTPLC_SCANNER_DEVICES="$S/devA.conf" \
  $B/e2e_two_plc scanner plcB 0xB2 500 > "$S/e2eB.log" 2>&1 &
sleep 1
ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
  $B/softplc-eip-scanner plcB "$S/devA.conf" > "$S/e2eB-eip.log" 2>&1 &

wait

echo
echo "================ RESULT ================"
grep -h "sent=" "$S/e2eA.log" "$S/e2eB.log" 2>/dev/null
grep -h "health byte" "$S/e2eB.log" 2>/dev/null
echo
echo "Pass criterion: each side's last_input is the OTHER side's pattern"
echo "(A sends 0xA1 and must see 0xB2; B sends 0xB2 and must see 0xA1),"
echo "and timeouts after warm-up are ~0."
echo "Logs: $S/e2e*.log"
