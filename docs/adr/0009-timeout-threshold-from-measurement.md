# 0009 — The failsafe threshold, set from measurement

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

20 000 exchanges at a 10 ms period, with the shipped 5 ms budget:

| | p50 | p99 | p99.9 | p99.99 | max | timeouts | **longest run** |
|---|---|---|---|---|---|---|---|
| idle, 10 ms | 170 µs | 328 µs | 687 µs | 4534 µs | 23.3 ms | 15 / 19 985 | **3** |

Supporting runs:

| | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| under load, 10 ms | 8 µs | 65 µs | 3.9 ms | 4.0 ms |
| idle, 1 ms | 128 µs | 300 µs | 656 µs | 15.0 ms |

## Decision

**Threshold 3 → 5.** Budget stays at 5000 µs.

## Consequences

The measurement did not confirm the placeholder; it showed it was **wrong**. A
run of three consecutive timeouts occurred on an idle machine with a perfectly
healthy peer, from host scheduling alone. A threshold of 3 therefore applied
the failsafe policy to a working system — on a plant, outputs dropping to HOLD
or CLEAR for no reason at all. This was not a value that merely lacked
evidence; it sat exactly on the environmental limit.

Five buys margin over the observed three, and the margin is cheap because of
an asymmetry worth stating plainly: **below the threshold the behaviour is
already HOLD.** Raising it only extends how long a HOLD-configured adapter
holds — no behavioural change at all. It matters materially only for
CLEAR-configured adapters, where crossing the threshold zeroes a live image,
which is precisely where a false trip does the most damage. The trade is
therefore heavily one-sided in favour of the higher number.

Detection latency is `threshold × cycle` — 50 ms at the default 10 ms task.
That is the same order as CIP's own connection timeout multiplier, whose
minimum is 4 × RPI, so the runtime is not slower to notice a dead peer than
the protocol underneath it.

The budget stays where it was, and the measurement is what justifies it: p99.99
landed at 4534 µs, just inside 5 ms, so the budget sits at the knee of the
distribution. Widening it to swallow the 23 ms outliers would mean a
timing-out adapter consuming most of the cycle — a worse trade than absorbing
those outliers as held frames.

**A non-zero timeout count on a healthy system is expected, not a fault.**
0.075 % of scans in this run, every one absorbed by holding. Anyone reading
`plc_adapter_stats::timeouts` should compare it against that baseline rather
than against zero.

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

`SOFTPLC_EIP_TIMEOUT_THRESHOLD` and `SOFTPLC_SCANNER_TIMEOUT_THRESHOLD`
override the defaults without a rebuild, so re-tuning is a deployment change.
