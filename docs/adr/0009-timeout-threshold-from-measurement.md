# 0009 — The failsafe trigger, set from measurement — and changed from a count to a duration

*Status: accepted*

## Context

`consecutive_timeout_threshold` shipped at **3** and `exchange_timeout_us` at
**5000**, both labelled placeholders pending "the Phase 0 golden-data run".
[ADR 0005](0005-failsafe-policy.md) makes the threshold the thing that decides
when a peer counts as *gone* — so it is the value that decides when a plant's
outputs drop to their failsafe state. Leaving it to a guess was the largest
unquantified risk in the runtime.

## The measurement

`tools/bench_exchange.c` drives the real two-process path — core pushes a
frame, posts the doorbell, the adapter process wakes, swaps images and
replies, the core matches the sequence and copies the input image — and
reports the round-trip distribution plus the longest run of **consecutive**
timeouts.

That last figure is the one the threshold rests on. An isolated miss is
absorbed by holding; only a run should reach failsafe. Measuring the mean, or
even p99.9, would not have answered the question.

Five samples of 20 000 exchanges each — 100 000 in total — at a 10 ms period
with the shipped 5 ms budget:

| sample | p50 | p99 | p99.9 | worst RTT | timeouts | **longest run** |
|---|---|---|---|---|---|---|
| A | 170 µs | 328 µs | 687 µs | 23.3 ms | 15 | **3** |
| B | 149 µs | 301 µs | 675 µs | 26.8 ms | 16 | **5** |
| C | 155 µs | 330 µs | 520 µs | 29.4 ms | 2 | 2 |
| D | 159 µs | 321 µs | 659 µs | 33.4 ms | 6 | 3 |
| E | 152 µs | 289 µs | 477 µs | 11.6 ms | 9 | 2 |

Supporting runs:

| | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| under load, 10 ms | 8 µs | 65 µs | 3.9 ms | 4.0 ms |
| idle, 1 ms | 128 µs | 300 µs | 656 µs | 15.0 ms |

## Decision

**Replace the count with a duration.** `consecutive_timeout_threshold`
becomes `failsafe_timeout_us`, defaulting to **100 ms**. Budget stays at
5000 µs.

## Consequences

The measurement did not confirm the placeholder; it showed it was **wrong**,
and then it showed the replacement was wrong too.

Sample A produced a run of three consecutive timeouts — on an idle machine,
with a perfectly healthy peer, from host scheduling alone. The shipped
threshold was three, so the default applied the failsafe policy to a working
system. It was raised to five. Sample B then produced a run of **five**.

Two thresholds, both chosen from an observed maximum, both beaten by the next
sample. That is not bad luck; it is the method being wrong. **Timeouts are not
independent.** At a 0.08 % timeout rate, five in a row would have probability
≈ 3 × 10⁻¹⁶ if they were — it would never be seen. What actually happens is
that a host stall withholds data for tens of milliseconds and takes out every
scan inside it, so the run length is really `stall / cycle`. Chasing the
observed maximum of a heavy-tailed quantity does not converge.

So the count is the wrong shape. Two things follow from switching to a
duration:

* **It is the quantity anyone actually reasons about.** "Declare the peer gone
  after 100 ms without data" is a statement about the plant. "After 5 misses"
  is a statement about an implementation detail.
* **It stops silently changing meaning.** The same count of 5 tolerates 50 ms
  at a 10 ms cycle and 5 ms at 1 ms, so retuning the scan rate quietly retuned
  the failsafe behaviour with it — a latent trap for whoever changes
  `SOFTPLC_CYCLE_US` next. This is also what CIP itself does: its connection
  timeout is a multiplier on RPI, i.e. a time.

It is also less state, not more: one timestamp instead of a counter, and the
same helper (`plc_staleness_*`) shared by all three adapters so HOLD and CLEAR
cannot come to mean subtly different things per protocol.

**100 ms** is twice the worst disruption observed across 100 000 exchanges
(a run of 5 at a 10 ms period ≈ 50 ms). The margin is cheap because of an
asymmetry worth stating: **inside the window the behaviour is already HOLD.**
Widening it only extends how long a HOLD-configured adapter holds — no
behavioural change at all. It matters materially only for CLEAR-configured
adapters, where crossing it zeroes a live image, which is exactly where a
false trip does the most damage.

One thing this makes uncomfortable but honest: CIP's own minimum connection
timeout is 4 × RPI, which at a 10 ms RPI is 40 ms — **less than the 50 ms
disruption this host produces**. A real scanner talking to this machine would
drop its connections. That is a statement about the host, not the code, and it
is the clearest available evidence that a general-purpose cloud VM is not a
platform for hard 10 ms control.

**Since confirmed by measurement, not left as an argument.** Running the two
roles against each other at RPI 2 ms with the multiplier at CIP's minimum — an
8 ms budget — the link was torn down and rebuilt 14 times in 12 seconds and
delivered 1 fresh frame in 499 while still reporting `ONLINE`. Raising the
multiplier to 4 (a 128 ms budget) took it to a single connection, zero
timeouts and 486 fresh frames. The multiplier is a per-device field in the
scanner's table now, and its default should be 2 (×16) or higher rather than
CIP's minimum. See
[eip-interop-verification.md](../eip-interop-verification.md).

The budget stays where it was, and the measurement is what justifies it: p99.99
landed at 4534 µs, just inside 5 ms, so the budget sits at the knee of the
distribution. Widening it to swallow the 23 ms outliers would mean a
timing-out adapter consuming most of the cycle — a worse trade than absorbing
those outliers as held frames.

**A non-zero timeout count on a healthy system is expected, not a fault.**
Between 0.01 % and 0.08 % of scans across the five samples, every one absorbed
by holding. Anyone reading `plc_adapter_stats::timeouts` should compare it
against that baseline rather than against zero — `stale_for_us` is the field
that actually tracks the failsafe decision.

One counter-intuitive result is worth carrying forward: **load was ~19× faster
than idle at the median** (8 µs against 170 µs). Idle pays wake-from-idle
latency on every exchange while a busy peer is already on a CPU. Tuning these
values against an idle bench and calling the result conservative would be
exactly backwards.

## Re-measure on the target hardware

These numbers describe a shared cloud host. A tuned or PREEMPT_RT kernel would
show a far shorter tail and justify a lower threshold — which is worth having,
since it buys back detection latency. An oversubscribed host would need a
higher one. Run the harness rather than inheriting this number on faith:

```sh
cmake --build build --target bench_exchange
./build/bench_exchange ./build/softplc-eip-adapter 20000 10000 5000 target
```

Run it **several times**. One sample would have justified a threshold of 3
here, and a second sample proved that wrong. `max_run × cycle_us` is the
disruption to size the timeout against, and it varies enough between samples
that a single run does not characterise it.

`SOFTPLC_EIP_FAILSAFE_TIMEOUT_US` and `SOFTPLC_SCANNER_FAILSAFE_TIMEOUT_US`
override the defaults without a rebuild, so re-tuning is a deployment change.
