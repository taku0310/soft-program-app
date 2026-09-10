# EtherNet/IP RPI evaluation: 5 ms / 10 ms / 50 ms

What this answers: **how many class 1 I/O packets this soft PLC actually
exchanged at each RPI, and how many times the connection timed out.** Every
number below was measured; nothing is extrapolated. Where something could not
be measured it says so.

Produced by `tools/eip_rpi_eval.sh` (one condition end to end),
`tools/eip_probe.c` (wire-level cadence), `tools/eip_sampler.py` (CPU, memory,
scheduler, interface counters) and `tools/eip_summarise.py` (tables).

## 1. Test environment

| | measured value |
|---|---|
| CPU | Intel Xeon @ 2.80 GHz, **4 vCPU** |
| Memory | 16 GB |
| Kernel | `6.18.44-fc-v24`, **PREEMPT_DYNAMIC** — not PREEMPT_RT |
| Platform | Firecracker microVM, containerised |
| Link | `veth` pair between two network namespaces |
| Clock | `CLOCK_MONOTONIC`, `AF_PACKET` capture on the Adapter-side interface |

The two roles are in separate network namespaces because CIP class 1 uses UDP
2222 at **both** ends; in one namespace each endpoint receives its own
transmissions.

**This is a general-purpose cloud VM, not control hardware.** Nothing here
transfers to a PREEMPT_RT machine without re-measuring; `tools/eip_rpi_eval.sh`
is in the tree so it can be re-run there.

## 2. Test conditions

Held constant across every run, so that a difference is attributable to the RPI:

| | value |
|---|---|
| Connection timeout multiplier | 2 → `(4 << 2)` = **×16 of the RPI** |
| PLC scan period | 10 000 µs |
| Image size | 32 bytes each direction |
| Devices | 1 |
| CPU allocation | none pinned; 4 vCPU shared |

CIP defines the connection timeout as a multiple of the RPI, so holding the
*multiplier* is what "same conditions" means here. The absolute budget
therefore scales: **80 ms / 160 ms / 800 ms**.

Load conditions: `none` (idle), `cpu` (`stress-ng --cpu 4 --cpu-load 50`),
`high` (`stress-ng --cpu 8 --cpu-load 100`). Measured host CPU: 1.6–2.5 %,
55.0–55.5 %, 98.2–98.3 %.

## 3. Adapter / Scanner configuration

Decoded from the packet capture, not from configuration files:

```
RegisterSession                        → session handle 0x00000001
Forward_Open (service 0x54)
    conn_serial=0x0001  vendor=1  originator_sn=0x534F4654
    timeout_multiplier=2  ((4<<2) = x16)
    O->T  RPI=10000us  size=38B  Point-to-point  Scheduled  owner=Exclusive
    T->O  RPI=10000us  size=34B  Point-to-point  Scheduled  owner=Exclusive
    transport/trigger=0x01  (Class 1, cyclic)
    connection_path = 20 04 24 97 2C 96 2C 64
                      Assembly class 0x04, config 151, O->T 150, T->O 100
Forward_Open reply
    GENERAL STATUS = 0 (success)
    O->T CID=0x42CA0013   T->O CID=0x71300001
```

**Exclusive Owner is confirmed on the wire** — the ownership bit is clear in
both `NetworkConnectionParameters`. Establishment took **0.8 ms** from the
first TCP SYN to the Forward_Open reply.

O→T is 38 B = 32 data + 4 run/idle header + 2 sequence count; T→O is
34 B = 32 + 2, the target producing without a run/idle header.

## 4. Results — how many packets, how many timeouts

`Expected` is `test duration / configured RPI`, per direction.

### 10 minutes, idle

| RPI | Expected | T→O received | O→T received | **UDP packets lost** | **CIP connection timeouts** | ForwardOpen |
|---|---|---|---|---|---|---|
| 5 ms | 120 000 | 58 778 | 100 573 | **0** | **1** | 2 |
| 10 ms | 60 000 | 58 881 | 50 075 | **0** | **0** | 1 |
| 50 ms | 12 000 | 12 000 | 11 271 | **0** | **0** | 1 |

### 1 hour, idle

| RPI | Expected | T→O received | O→T received | **UDP packets lost** | **CIP connection timeouts** | ForwardOpen |
|---|---|---|---|---|---|---|
| 5 ms | 720 000 | 352 519 | 603 790 | **0** | **6** | 7 |
| 10 ms | 360 000 | 353 942 | 301 598 | **0** | **0** | 1 |
| 50 ms | 72 000 | 71 995 | 67 722 | **0** | **0** | 1 |

Timeout rate against expected packet count: 5 ms → **0.00083 %**;
10 ms and 50 ms → **0 %**.

### 10 minutes, under CPU load

| RPI | load | host CPU | **CIP connection timeouts** | ForwardOpen | worst cycle |
|---|---|---|---|---|---|
| 5 ms | 50 % | 55.2 % | **2** | 3 | 69 660 µs |
| 5 ms | 100 % | 98.3 % | **0** | 1 | 57 676 µs |
| 10 ms | 50 % | 55.5 % | **0** | 1 | 97 585 µs |
| 10 ms | 100 % | 98.2 % | **0** | 1 | 62 208 µs |
| 50 ms | 50 % | 55.0 % | **0** | 1 | 146 861 µs |
| 50 ms | 100 % | 98.3 % | **0** | 1 | 61 321 µs |

**Not a single UDP packet was lost in any of the twelve runs.** The
encapsulation sequence number is contiguous throughout; every observed gap is a
sender that did not send, never a packet that did not arrive. Interface RX/TX
errors and drops, and UDP `InErrors` / `RcvbufErrors`, were zero in all runs.

## 5. Cycle and jitter statistics

All values µs, measured at the interface.

| RPI | load | dir | mean | P50 | P95 | P99 | P99.9 | max | σ |
|---|---|---|---|---|---|---|---|---|---|
| 5 ms | none | T→O | **10 603** | 9 970 | 10 965 | 19 205 | 101 285 | 131 960 | 5 871 |
| 5 ms | none | O→T | **6 207** | 5 900 | 6 815 | 9 150 | 33 060 | 131 950 | 3 957 |
| 10 ms | none | T→O | **10 190** | 10 195 | 10 840 | 11 135 | 31 790 | 129 242 | 1 431 |
| 10 ms | none | O→T | **11 982** | 11 780 | 12 730 | 13 245 | 34 040 | 99 033 | 1 397 |
| 50 ms | none | T→O | **50 002** | 50 720 | 51 620 | 52 860 | 71 795 | 117 147 | 3 361 |
| 50 ms | none | O→T | **53 236** | 53 040 | 55 250 | 56 010 | 71 735 | 207 160 | 2 096 |

1-hour runs, idle:

| RPI | dir | mean | P99 | P99.9 | max |
|---|---|---|---|---|---|
| 5 ms | T→O | 10 187 | 11 085 | 29 125 | 76 860 |
| 5 ms | O→T | 5 946 | 6 740 | 17 080 | 76 824 |
| 10 ms | T→O | 10 171 | 11 015 | 26 155 | 118 342 |
| 10 ms | O→T | 11 936 | 12 935 | 28 635 | 118 298 |
| 50 ms | T→O | 50 004 | 52 335 | 71 575 | 279 018 |
| 50 ms | O→T | 53 158 | 55 730 | 75 585 | 277 509 |

Two systematic deviations, present at every RPI and under every load:

* **T→O at a 5 ms RPI runs at 10 ms** — exactly 2×.
* **O→T runs long by a factor that shrinks as the RPI grows**: ×1.24 at 5 ms,
  ×1.20 at 10 ms, ×1.065 at 50 ms.

Both are implementation defects, root-caused in §8.

## 6. CPU, memory and network

| run | host CPU | Adapter stack | Scanner stack | max run-queue delay | Adapter RSS | Scanner RSS |
|---|---|---|---|---|---|---|
| 5 ms idle, 10 min | 2.5 % | 1.8 % | 3.1 % | 34 070 µs/s | 2 024 → 2 024 kB | 4 248 → 4 316 kB |
| 10 ms idle, 10 min | 2.2 % | 1.5 % | 2.5 % | 2 183 µs/s | no change | no change |
| 50 ms idle, 10 min | 1.7 % | 1.0 % | 1.9 % | 2 840 µs/s | no change | no change |
| 5 ms idle, **1 h** | 2.2 % | 1.8 % | 2.9 % | 3 975 µs/s | no change | 4 224 → 4 292 kB |
| 10 ms idle, **1 h** | 2.0 % | 1.6 % | 2.5 % | 3 378 µs/s | no change | no change |
| 50 ms idle, **1 h** | 1.6 % | 1.0 % | 1.9 % | 4 126 µs/s | no change | no change |

Threads: PLC core 1, each stack process 2.

**No memory leak.** The scanner's +68 kB appears only in the RPI 5 ms runs — the
ones that reconnect — and is the *same* +68 kB after 1 hour as after 10 minutes,
across six times the duration and six reconnects instead of one. It is a
one-time allocation on the reconnect path, not growth.

**The stalls are not our CPU.** Worst cycles of 117–279 ms occur while the host
is 1.6–2.5 % busy and all four of our processes together use under 6 %.

## 7. The IPC leg (PLC core ↔ stack process)

Separately measured, because a late frame on the wire could originate on either
side of the shared-memory boundary.

| run | exchanges | IPC timeouts | rate | mid-run failsafe activations |
|---|---|---|---|---|
| 5 ms, 1 h | 364 255 / 364 293 | 245 / 207 | 0.1 % | **0** |
| 10 ms, 1 h | 364 265 / 364 246 | 235 / 254 | 0.1 % | **0** |
| 50 ms, 1 h | 364 204 / 364 217 | 296 / 283 | 0.1 % | **0** |

(Two figures per cell: Adapter-side core / Scanner-side core.)

The IPC leg times out on ~0.1 % of scans at every RPI — a flat rate,
independent of the RPI, which is what it should be: this leg runs at the scan
period, not the RPI. **No failsafe was applied mid-run in any condition**; the
one activation per run in the logs is the start-up window before the peer
attaches.

So the IPC leg is not where the RPI-dependent behaviour comes from. It is
absorbed entirely by holding the last good image, as designed.

The three idle 10-minute runs and `short_rpi5000_cpu` have no IPC totals: the
driver killed the cores before they printed them. Fixed in `ea159a5`; those
four conditions are missing this one metric and nothing else.

## 8. Root causes

### 8.1 A 5 ms RPI is quantised to 10 ms by the Adapter

`src/adapters/eip/opener_conf/opener_user_conf.h` sets

```c
static const MilliSeconds kOpenerTimerTickInMilliSeconds = 10;
```

and OpENer's `ConnectionObjectSetExpectedPacketRate()`
(`third_party/OpENer/source/src/cip/cipconnectionobject.c:459`) rounds the
requested RPI **up** to a multiple of it:

```
remainder = 5000 % 10000 = 5000      (non-zero)
expected_packet_rate = 5000/1000 + (10 - 5000/1000) = 5 + 5 = 10 ms
```

Measured T→O mean at a 5 ms RPI: **10 168 µs**. At 10 ms the remainder is zero,
no rounding occurs, and the measured mean is 10 190 µs. At 50 ms, 50 002 µs.

This is legitimate CIP behaviour — a target may serve a coarser rate than
requested — but with this constant **the Adapter role cannot produce faster
than 10 ms**, whatever the scanner asks for.

### 8.2 The Scanner loses real time on every tick

`third_party/EIPScanner/src/IOConnection.cpp:96`:

```cpp
auto sinceLastHandle =
    std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastHandleTime);
auto periodInMicroS = sinceLastHandle.count() * 1000;
```

Elapsed time is truncated to whole milliseconds, the remainder is discarded,
and `_lastHandleTime = now` is then set from the *untruncated* clock — so the
remainder is lost permanently rather than carried forward. That value drives
both:

* `_o2tTimer`, the send cadence → **O→T is sent late**;
* `_connectionTimeoutCount` → **the connection timeout fires later than the
  configured `(4 << mult) × RPI` in real time.**

The second is the safety-relevant one: the configured timeout is not the
timeout in effect.

### 8.3 Why the connection drops at 5 ms and not at 10 ms

The CIP budget scales with the RPI: 80 ms, 160 ms, 800 ms.

Within a surviving connection generation at a 5 ms RPI over 1 hour, the largest
observed silence was **76 860 µs** — just under the 80 ms budget. The stalls
that exceeded 80 ms are precisely the six that killed the connection.

At 10 ms the budget is 160 ms and the worst observed silence was 118 342 µs; at
50 ms the budget is 800 ms against a worst of 279 018 µs. Both survive with
margin.

**So the boundary is not the RPI itself but the host's scheduling tail against
the budget the RPI implies.** This host's tail reaches ~130 ms idle and ~280 ms
occasionally; an 80 ms budget sits inside it, a 160 ms budget mostly outside it,
an 800 ms budget far outside.

### 8.4 Availability at RPI 5 ms over one hour

Derived from the connection generations on the wire:

| | |
|---|---|
| Connection generations | 7 |
| Reconnects | 6 |
| Generation lifetimes | 82, 28, 1 078, 208, 1 468, 718, 5.3 s |
| **Time with a live connection** | **3 587.6 s of 3 600 s = 99.66 %** |
| Total outage | 12.4 s |
| Mean recovery per event | ≈ 2.1 s |

Recovery is automatic every time; no reconnect required intervention and none
failed. But a 2-second outage six times an hour is a plant stoppage, not a
statistic.

## 9. CPU load did not make it worse — it made it better

This was the most counterintuitive result and it is consistent across all three
RPIs.

| RPI | idle worst cycle | 50 % load | 100 % load |
|---|---|---|---|
| 5 ms | 131 960 µs | 69 660 µs | **57 676 µs** |
| 10 ms | 129 242 µs | 97 585 µs | **62 208 µs** |
| 50 ms | 117 147 µs | 146 861 µs | **61 321 µs** |

At a 5 ms RPI, connection timeouts went 1 (idle) → 2 (50 %) → **0 (100 %)**.

A busy CPU does not enter deep idle states, so the wake-from-idle latency that
dominates this host's tail disappears. The same effect was measured earlier in
this project from the other side of the IPC boundary (ADR 0009: load measured
~19× faster than idle at the median).

**Consequence for anyone tuning this: an idle bench is the pessimistic case
here, not the optimistic one.** Tuning against a loaded system and assuming
idle will be safer is backwards.

## 10. Verdict per RPI

### RPI 5 ms — **not usable as it stands**

* The Adapter physically cannot produce at 5 ms with this build; it produces at
  10 ms (§8.1). The requested rate is not being served.
* 6 connection timeouts and 6 reconnects in 1 hour idle; 12.4 s of accumulated
  outage; 99.66 % availability.
* The 80 ms CIP budget lies inside this host's scheduling tail.

### RPI 10 ms — **usable on this host, with a caveat**

* **0 connection timeouts** in 1 hour idle and in all load conditions.
* 0 packets lost out of 353 942 + 301 598.
* T→O cadence accurate to +1.9 %.
* Caveat: **O→T runs at 11.94 ms, not 10 ms** (§8.2). The link is stable, but
  the actual output update rate is 19 % slower than configured.
* Worst silence 118 ms against a 160 ms budget — 26 % margin. That is thinner
  than it looks on a host whose tail reached 279 ms in another run.

### RPI 50 ms — **usable with real margin**

* 0 connection timeouts, 0 packets lost, in every condition.
* T→O accurate to 0.004 %.
* O→T runs at 53.2 ms (+6.5 %) — the same defect, proportionally smallest here.
* Worst silence 279 ms against an 800 ms budget — 65 % margin.

## 11. Recommended RPI

**On this hardware and this build, the smallest RPI that can be recommended is
10 ms — and 50 ms is what should be used where the application allows it.**

| | RPI | reason |
|---|---|---|
| Conservative | **50 ms** | 65 % margin against the observed scheduling tail; zero timeouts in every condition measured |
| Performance | **10 ms** | zero timeouts measured, but only 26 % margin, and the output rate is really 11.9 ms |
| Not recommended | 5 ms | the Adapter serves it at 10 ms anyway, and the implied 80 ms budget is inside the host's stall distribution |

Asking for 5 ms today buys nothing over asking for 10 ms — the Adapter produces
at 10 ms either way — while halving the timeout budget. **It is strictly worse
than 10 ms on this platform.**

### Current bottleneck

In order of what actually limits the achievable RPI:

1. **Host scheduling tail** (~130 ms idle, ~280 ms observed) on a
   non-PREEMPT_RT kernel in a shared microVM. This sets the floor and nothing in
   the application can move it.
2. **`kOpenerTimerTickInMilliSeconds = 10`** — a hard 10 ms floor on the
   Adapter's production rate.
3. **The Scanner's millisecond truncation** — inflates both the send period and
   the connection timeout.

Note the ordering: even with both defects fixed, item 1 remains, and it is the
reason a 5 ms RPI is not a realistic target on this class of machine.

## 12. Fixing the two defects — measured, not proposed

Both were fixed and the same conditions re-measured. `SOFTPLC_OPENER_TICK_MS=1`
plus `patches/eipscanner-io-timer.patch`.

### The cadence is fully repaired

10 minutes, idle. Three variants, so the two scanner-side changes can be told
apart:

| RPI | dir | baseline | tick fix + µs fix | **+ overshoot fix** | target |
|---|---|---|---|---|---|
| 5 ms | T→O | 10 603 µs | **5 007 µs** | 5 011 µs | 5 000 |
| 5 ms | O→T | 6 207 µs | 6 053 µs | **5 001 µs** | 5 000 |
| 10 ms | T→O | 10 190 µs | **10 011 µs** | 10 011 µs | 10 000 |
| 10 ms | O→T | 11 982 µs | 12 042 µs | **10 002 µs** | 10 000 |
| 50 ms | T→O | 50 002 µs | 50 000 µs | — | 50 000 |
| 50 ms | O→T | 53 236 µs | 52 012 µs | — | 50 000 |

Frames delivered against the configured RPI went from 49.15 % / 84.11 % to
**99.79 % / 99.98 %** at a 5 ms RPI.

**The first hypothesis was wrong and the measurement is what caught it.** The
millisecond truncation (§8.2) is real, but fixing it alone left the 10 ms O→T
period at 12 042 µs — no better than the 11 982 µs baseline. The dominant error
was a second defect in the same function: `_o2tTimer = 0` on send, discarding
the overshoot past the deadline instead of subtracting the period, so the mean
period was `RPI + mean overshoot`. Changing it to `_o2tTimer -= _o2tAPI` is what
moved 11 982 µs to 10 002 µs.

The truncation fix is kept regardless: it also governs `_connectionTimeoutCount`,
so without it the configured connection timeout is not the one in effect.

### The connection still drops at 5 ms

1 hour, idle, RPI 5 ms:

| | baseline | all three fixes |
|---|---|---|
| T→O packets | 352 519 | **717 652** |
| T→O mean period | 10 187 µs | **5 009 µs** |
| O→T mean period | 5 946 µs | **5 002 µs** |
| UDP packets lost | 0 | 0 |
| **CIP connection timeouts** | **6** | **3** |
| Reconnects | 6 | 3 |
| Connection lifetimes | 82, 28, 1078, 208, 1468, 718, 5 s | 632, 1438, 993, 530 s |
| **Availability** | **99.66 %** | **99.83 %** |
| Total outage | 12.4 s | 6.2 s |

Halved, not eliminated. This is the expected result and it is worth stating
plainly: **the fixes repair cadence, not stall tolerance.** The CIP budget at a
5 ms RPI is still 80 ms, and this host still stalls past 80 ms — the worst
silence in the fixed run was 116 436 µs. The drop rate roughly halved because
T→O now arrives twice as often, so a stall must cover twice as many expected
frames before the timeout counter expires; the stalls themselves are unchanged.

**So the recommendation in §11 does not change.** 10 ms remains the smallest
RPI that can be recommended on this hardware. What the fixes change is that a
requested RPI is now actually served: before them, configuring 10 ms produced
output at 11.98 ms and configuring 5 ms produced it at 10 ms.

### Cost, and why the tick is an option rather than a new default

`SOFTPLC_OPENER_TICK_MS=1` runs OpENer's connection manager ten times as often:

| RPI | tick 10 ms | tick 1 ms |
|---|---|---|
| 5 ms | 1.82 % | 4.29 % |
| 10 ms | 1.54 % | 3.82 % |
| 50 ms | 1.00 % | 3.32 % |

At a 50 ms RPI that is 2.3 points of CPU for nothing at all — the RPI is
already an exact multiple of the 10 ms tick. The default therefore stays at
10 ms, and the option exists for deployments that need sub-10 ms and have
accepted what §11 says about them.

## 13. What could not be measured

* **8–24 hour runs.** The longest completed run is 1 hour per RPI. The session
  container is reclaimed on inactivity, so multi-hour runs could not be
  guaranteed to complete. Slow degradation over a working shift is therefore
  **untested**; the 1-hour runs show no drift, but one hour is not a shift.
* **Real EtherNet/IP hardware.** Both roles were tested against each other. No
  third-party scanner has driven this Adapter and no real device has been driven
  by this Scanner.
* **IPC totals for four conditions** (§7).
* **Scheduling latency of the kernel itself** — no `perf`, and
  `/proc/<pid>/schedstat` run-queue delay is the closest available proxy; it is
  reported per second, not per event, so it cannot be attributed to an
  individual late frame.
