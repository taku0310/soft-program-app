#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# One measurement run: a soft PLC in the Adapter role and one in the Scanner
# role, in separate network namespaces, exchanging Exclusive Owner class 1 I/O
# at a given RPI for a given time, with the wire, both PLC cores and the host
# all instrumented.
#
#   eip_rpi_eval.sh <build> <outdir> <rpi_us> <seconds> <none|cpu|high> <label>
#
# Only the RPI and the load are meant to vary between runs. The scan period,
# image sizes, connection timeout multiplier, CPU allocation and namespace
# layout are fixed here so that a difference between two runs is attributable.
#
# The connection timeout multiplier stays at 2 across every RPI on purpose.
# CIP defines the timeout as a multiple of the RPI, so holding the multiplier
# is what "same conditions" means for this protocol - the absolute budget then
# scales with the RPI, which is the behaviour under test.

set -u
B="${1:?build dir}"
OUT="${2:?out dir}"
RPI_US="${3:?rpi us}"
SECS="${4:?seconds}"
LOAD="${5:-none}"
LABEL="${6:-run}"

TMO_MULT=2                       # (4 << 2) = x16 of the RPI
SCAN_US=10000                    # PLC scan period, fixed
IMG=32                           # bytes each way, fixed
BUDGET_US=$(( (4 << TMO_MULT) * RPI_US ))

mkdir -p "$OUT"
W="$OUT/work"; mkdir -p "$W"

log() { printf '%s | %s\n' "$(date -u +%H:%M:%S)" "$*"; }

cleanup() {
  for pid in ${PIDS:-}; do kill -TERM "$pid" 2>/dev/null; done
  [ -n "${STRESS_PID:-}" ] && kill -TERM "$STRESS_PID" 2>/dev/null
  sleep 0.5
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc; do
    for p in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
  done
  ip netns del plcA 2>/dev/null; ip netns del plcB 2>/dev/null
  rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*
}
trap cleanup EXIT

# --- two "machines" -------------------------------------------------------
ip netns del plcA 2>/dev/null; ip netns del plcB 2>/dev/null
ip link del vethA 2>/dev/null
ip netns add plcA && ip netns add plcB || { echo "netns failed" >&2; exit 1; }
ip link add vethA type veth peer name vethB
ip link set vethA netns plcA
ip link set vethB netns plcB
ip netns exec plcA ip addr add 10.10.0.1/24 dev vethA
ip netns exec plcA ip link set vethA up
ip netns exec plcA ip link set lo up
ip netns exec plcB ip addr add 10.10.0.2/24 dev vethB
ip netns exec plcB ip link set vethB up
ip netns exec plcB ip link set lo up
# The veth default of 1000 is far above anything a 32-byte class 1 stream
# needs; left alone so it is not a variable.
ip netns exec plcA ip link show vethA > "$OUT/link.txt" 2>&1

for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc; do
  for p in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
done
rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*
sleep 0.5

cat > "$W/devA.conf" <<EOF
# ip          cfg o2t t2o o2t_len t2o_len rpi_us   tmo_mult failsafe
10.10.0.1     151 150 100 $IMG    $IMG    $RPI_US  $TMO_MULT hold
EOF

# Scans are sized to outlast the measurement window by a wide margin: whichever
# PLC exits first takes its stack down and would end the other's connection
# inside the window.
SCANS=$(( (SECS + 180) * 1000000 / SCAN_US ))

log "RPI=${RPI_US}us  budget=${BUDGET_US}us  secs=${SECS}  load=${LOAD}  label=${LABEL}"

# --- PLC A: Adapter (CIP target) -----------------------------------------
ip netns exec plcA env SOFTPLC_LOG_LEVEL=info \
  "$B/e2e_two_plc" adapter plcA 0xA1 "$SCANS" > "$OUT/plcA-core.log" 2>&1 &
A_CORE=$!
sleep 1
ip netns exec plcA "$B/softplc-eip-adapter" plcA vethA > "$OUT/plcA-stack.log" 2>&1 &
A_STACK=$!
sleep 2

# --- PLC B: Scanner (CIP originator) -------------------------------------
ip netns exec plcB env SOFTPLC_LOG_LEVEL=info SOFTPLC_SCANNER_DEVICES="$W/devA.conf" \
  "$B/e2e_two_plc" scanner plcB 0xB2 "$SCANS" > "$OUT/plcB-core.log" 2>&1 &
B_CORE=$!
sleep 1
ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
  "$B/softplc-eip-scanner" plcB "$W/devA.conf" > "$OUT/plcB-stack.log" 2>&1 &
B_STACK=$!
PIDS="$A_CORE $A_STACK $B_CORE $B_STACK"

# --- wait for the Exclusive Owner connection to be up ---------------------
# Measuring from t=0 would fold ForwardOpen into the cadence statistics and
# make every run look like it had one enormous gap.
for i in $(seq 1 60); do
  grep -q "Open IO connection" "$OUT/plcB-stack.log" 2>/dev/null && break
  sleep 0.5
done
if ! grep -q "Open IO connection" "$OUT/plcB-stack.log" 2>/dev/null; then
  log "ERROR: no ForwardOpen within 30 s"
  tail -20 "$OUT/plcB-stack.log" 2>/dev/null
  exit 1
fi
log "connection established; settling 3 s then measuring"
sleep 3

# --- load ----------------------------------------------------------------
NCPU=$(nproc)
case "$LOAD" in
  none) : ;;
  cpu)  stress-ng --cpu "$NCPU" --cpu-load 50 --timeout $((SECS + 30))s \
          > "$OUT/stress.log" 2>&1 & STRESS_PID=$! ;;
  high) stress-ng --cpu $((NCPU * 2)) --cpu-load 100 --timeout $((SECS + 30))s \
          > "$OUT/stress.log" 2>&1 & STRESS_PID=$! ;;
  *) echo "unknown load '$LOAD'" >&2; exit 2 ;;
esac
[ "$LOAD" != "none" ] && log "load '$LOAD' started (pid ${STRESS_PID:-?})"

# --- measure -------------------------------------------------------------
python3 tools/eip_sampler.py "$SECS" "$OUT/resources.csv" plcA,plcB -- \
  plcA_core=$A_CORE plcA_stack=$A_STACK plcB_core=$B_CORE plcB_stack=$B_STACK \
  > "$OUT/sampler.log" 2>&1 &
SAMPLER=$!

ip netns exec plcA "$B/eip_probe" vethA "$SECS" "$RPI_US" "$BUDGET_US" \
  "$LABEL" > "$OUT/probe.txt" 2>&1
log "probe finished"

wait $SAMPLER 2>/dev/null

# --- collect -------------------------------------------------------------
{
  echo "label=$LABEL rpi_us=$RPI_US seconds=$SECS load=$LOAD"
  echo "tmo_mult=$TMO_MULT budget_us=$BUDGET_US scan_us=$SCAN_US img_bytes=$IMG"
  echo "ncpu=$NCPU kernel=$(uname -r)"
  echo "forward_open_count=$(grep -c 'Open IO connection' "$OUT/plcB-stack.log" 2>/dev/null)"
  echo "conn_closed_count=$(grep -ci 'close\|timeout\|lost' "$OUT/plcB-stack.log" 2>/dev/null)"
} > "$OUT/meta.txt"

ip netns exec plcA cat /proc/net/snmp > "$OUT/plcA-snmp.txt" 2>/dev/null
ip netns exec plcB cat /proc/net/snmp > "$OUT/plcB-snmp.txt" 2>/dev/null
ip netns exec plcA cat /proc/net/dev  > "$OUT/plcA-dev.txt"  2>/dev/null
ip netns exec plcB cat /proc/net/dev  > "$OUT/plcB-dev.txt"  2>/dev/null
ip netns exec plcB ss -uan            > "$OUT/plcB-sockets.txt" 2>/dev/null

log "done -> $OUT"
