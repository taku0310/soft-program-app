#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance check B6: does Exclusive Owner survive a saturated link?
#
# The link is capped with a token bucket (tbf) so saturation is a controlled
# condition rather than a race against the host's CPU. netem is absent on this
# kernel, but tbf and htb are present - so bandwidth can be constrained even
# though delay and reordering cannot, which is why those are injected by
# tools/cip_impair.c instead.
#
# Three namespaces on one bridge: the two PLCs and a third host whose only job
# is to fill the pipe. The cap sits on the bridge port facing the scanner, so
# the flood and the CIP traffic contend for the same tokens.
#
#   sudo tools/e2e_bandwidth.sh [build-dir] [work-dir] [rate] [seconds]
#
# At a 10 ms RPI the I/O itself is about 120 kbit/s in each direction, so a
# 2 Mbit cap is roughly sixteen times what the connection needs: what breaks
# it is the queue in front of it, not the bandwidth it consumes.

set -u
FLOOD_TX=""
FLOOD_RX=""
B="${1:-./build-scan}"
W="${2:-/tmp/softplc-bw}"
RATE="${3:-2mbit}"
SECS="${4:-25}"
mkdir -p "$W"

cleanup() {
  for exe in softplc-eip-adapter softplc-eip-scanner e2e_two_plc; do
    for pid in $(pgrep -f "$B/$exe" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  done
  # By pid, not by pattern: `pkill -f` matches any command line containing
  # the pattern, and the shell invoking this script is one of them - a driver
  # that kills its own caller is a hard failure to read.
  [ -n "${FLOOD_TX:-}" ] && kill -9 "$FLOOD_TX" 2>/dev/null
  [ -n "${FLOOD_RX:-}" ] && kill -9 "$FLOOD_RX" 2>/dev/null
  FLOOD_TX=""; FLOOD_RX=""
  for ns in plcA plcB load; do
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

run_case() {
  local name="$1" flood="$2"
  local log="$W/$name"
  mkdir -p "$log"
  cleanup

  ip netns add plcA && ip netns add plcB && ip netns add load || return 1
  ip link add br0 type bridge && ip link set br0 up
  for ns in plcA plcB load; do
    ip link add "veth-$ns" type veth peer name "in-$ns"
    ip link set "veth-$ns" master br0
    ip link set "veth-$ns" up
    ip link set "in-$ns" netns "$ns"
    ip netns exec "$ns" ip link set lo up
    ip netns exec "$ns" ip link set "in-$ns" up
  done
  ip netns exec plcA ip addr add 10.10.0.1/24 dev in-plcA
  ip netns exec plcB ip addr add 10.10.0.2/24 dev in-plcB
  ip netns exec load ip addr add 10.10.0.3/24 dev in-load

  # The bottleneck: everything the scanner receives crosses this.
  tc qdisc add dev veth-plcB root tbf rate "$RATE" burst 32kbit latency 50ms \
    || { echo "tbf unavailable"; return 1; }
  rm -f /dev/shm/softplc.* /dev/shm/sem.softplc.*

  printf '10.10.0.1 151 150 100 32 32 10000 2 hold\n' > "$log/dev.conf"

  ip netns exec plcA $B/e2e_two_plc adapter plcA 0xA1 100000 > "$log/A.log" 2>&1 &
  sleep 1
  ip netns exec plcA $B/softplc-eip-adapter plcA in-plcA > "$log/A-eip.log" 2>&1 &
  sleep 2
  local scans=$(( (SECS - 5) * 100 ))
  ip netns exec plcB env SOFTPLC_SCANNER_DEVICES="$log/dev.conf" \
    $B/e2e_two_plc scanner plcB 0xB2 "$scans" > "$log/B.log" 2>&1 &
  local verifier=$!
  sleep 1
  ip netns exec plcB env SOFTPLC_SCANNER_STACK_LOG=info \
    $B/softplc-eip-scanner plcB "$log/dev.conf" > "$log/B-eip.log" 2>&1 &

  if [ "$flood" = "1" ]; then
    sleep 3
    # Something has to be listening, or the first datagram draws an ICMP port
    # unreachable and the sender's connected UDP socket gives up - which looks
    # like a saturated link that somehow carried almost no traffic.
    ip netns exec plcB sh -c 'nc -u -l -p 9999 > /dev/null 2>&1' &
    FLOOD_RX=$!
    sleep 1
    # Normal-sized datagrams, not whatever nc happens to read: piping
    # /dev/zero straight in produces datagrams far larger than the token
    # bucket's burst, and tbf drops an oversized packet outright instead of
    # queueing it. The flood then consumed no bandwidth at all and the
    # "saturated" link carried the I/O untouched - a green result measuring
    # nothing. dd fixes the read size, so each datagram fits the bucket and
    # has to be paid for in tokens.
    ip netns exec load sh -c \
      'dd if=/dev/zero bs=1400 2>/dev/null | nc -u -w 60 10.10.0.2 9999' \
      > /dev/null 2>&1 &
    FLOOD_TX=$!
  fi

  wait $verifier 2>/dev/null
  local rc=$?
  echo "  $(grep -hE 'tag=0xB2  warmup' "$log/B.log" | head -1)"
  echo "  $(grep -hE 'peer_tag=' "$log/B.log" | head -1)"
  echo "  ForwardOpens=$(grep -c 'Open IO connection' "$log/B-eip.log" 2>/dev/null)" \
       "closes=$(grep -c 'is closed by timeout' "$log/B-eip.log" 2>/dev/null)"
  # What the bottleneck actually carried, so "saturated" is a measurement
  # rather than an intention.
  local stats bytes pkts
  stats=$(tc -s qdisc show dev veth-plcB | tr '\n' ' ')
  bytes=$(echo "$stats" | sed -n 's/.*Sent \([0-9]*\) bytes.*/\1/p')
  pkts=$(echo "$stats" | sed -n 's/.*bytes \([0-9]*\) pkt.*/\1/p')
  echo "  through the cap: ${bytes} bytes / ${pkts} pkt" \
       "= $(( bytes * 8 / (SECS > 0 ? SECS : 1) / 1000 )) kbit/s;" \
       "$(echo "$stats" | sed -n 's/.*\(dropped [0-9]*\).*/\1/p')" 
  echo
  return $rc
}

echo "=== link capped at $RATE, no competing traffic ==="
run_case quiet 0
echo "=== same cap, a third host filling the pipe ==="
run_case flooded 1
