# Verifying EtherNet/IP between two soft PLCs

Until this was run, "the OpENer and EIPScanner integrations work" rested on
each stack starting up and on IPC tests with mirror backends. No CIP frame had
ever crossed a wire. This closes that gap: two soft PLCs, one in each role,
exchanging real class 1 cyclic I/O.

```sh
cmake -S . -B build-scan -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON
cmake --build build-scan -j
sudo tools/e2e_two_plc.sh
```

## What it does

| | PLC A | PLC B |
|---|---|---|
| role | Adapter (CIP target) | Scanner (originator) |
| stack | OpENer | EIPScanner |
| address | 10.10.0.1 | 10.10.0.2 |
| sends | `0xA1` on every output byte | `0xB2` |
| must receive | `0xB2` | `0xA1` |

Each side sends a *different* pattern, so "A sees B's pattern and B sees A's"
cannot be produced by a loopback, a stuck buffer, or an echo. It requires
payload to have crossed in both directions.

## Result

```
adapter  sent=0xA1  warmup_scans=97  exchanges=1200 timeouts=0 (0.0%)
         peer_data_scans=798  last_input=0xB2  max_rtt=825us   state=ONLINE
scanner  sent=0xB2  warmup_scans=98  exchanges=500  timeouts=0 (0.0%)
         peer_data_scans=500  last_input=0xA1  max_rtt=2867us  state=ONLINE
scanner  device health: reached online during the run = yes
```

Bidirectional, **zero timeouts** after warm-up, and the scanner saw peer data
on every one of its 500 scans with the per-device health block reporting the
device online.

PLC A deliberately runs longer than B: whichever side exits first unlinks its
IPC objects and takes its stack down with it, which drops the other's CIP
connection. An earlier version had B outliving A and duly reported a final
health byte of 2 (`FAILSAFE`) — describing the teardown, not the run. The
harness now reports whether the device *reached* online during the run, which
is the question actually being asked.

The assembly numbering lines up without configuration because the two roles'
defaults were designed against each other: the Adapter creates produced=100
(T→O), consumed=150 (O→T) and config=151, and the Scanner's device table
addresses exactly those.

## The two roles cannot share a network namespace

CIP class 1 uses a **fixed UDP port, 2222, at both ends**. Two EtherNet/IP
endpoints on one host therefore contend for it, and each ends up receiving its
own transmissions. The symptom is unmistakable once you know it:

```
[INFO]  Open IO connection O2T_ID=3895525395 T2O_ID=1336016897
[ERROR] Received data from unknown connection T2O_ID=3895525395
```

The ID in the error is the connection's **O2T** id — the one we send *with* —
so the scanner is reading back its own outgoing frames. On a real line the two
roles are on different machines and the question never arises; locally,
separate network namespaces are the stand-in, which is what the script builds.

Separate containers get separate namespaces by default, so
`docker/docker-compose.yml` is already correct — but do not collapse the two
services with `network_mode: service:`.

## What the frames carry, and why it is not a constant

The first version of this harness filled the output image with a single
repeated byte. That is enough to prove "A sees B and B sees A", and nothing
else — a constant fill passes unchanged through a stale frame, a repeated
frame, or a one-byte shift in the image map.

Each side now sends a 32-bit sequence number followed by bytes derived from
their own index and the sender's tag, which separates three faults a constant
could not:

| fault | how it shows |
|---|---|
| loopback, echo, wrong peer | `peer_tag` is not the other side's |
| stale or repeated frame | the sequence stops advancing (`fresh` vs `stale`) |
| byte-offset error in the map | the per-index bytes do not match (`corrupt`) |

`corrupt = 0` in both directions across the whole 32-byte image is information
the earlier constant-fill run simply did not contain.

## Losing and regaining the connection

The script kills PLC A's CIP stack mid-run and restarts it. Nothing else in
the suite reaches those paths: the mirror backends never lose a connection, so
OpENer's `CheckIoConnectionEvent` on the target side and the scanner's close
listener and gated reconnect had never executed.

They work. The scanner logs the connection closing, retries on its 2 s gate,
and reopens — the ForwardOpen count goes above one and data resumes. The
adapter side meanwhile times out for exactly the ~300 scans its stack was
down (3 s at a 10 ms task) and returns to `ONLINE`.

## RPI, the CIP timeout multiplier, and this host

`connectionTimeoutMultiplier` was left at 0. That is CIP's **minimum**: the
target drops the link after `(4 << mult) × RPI` without a frame, so 0 means 4 ×
RPI. It is now a per-device field in the table, because measurement showed the
minimum is unusable here.

At RPI 2 ms, multiplier 0 gives an 8 ms budget:

| RPI 2 ms | multiplier 0 (8 ms) | multiplier 4 (128 ms) |
|---|---|---|
| ForwardOpens in ~12 s | **15** (14 reconnects) | **1** |
| connection closed by timeout | 14 | **0** |
| scanner fresh / stale frames | 1 / 498 | **486 / 12** |

The link spent the whole run being torn down and rebuilt, delivering almost no
fresh data while still reporting `ONLINE` — the failure mode is quiet, which is
what makes it worth a test.

This is the prediction in [ADR 0009](adr/0009-timeout-threshold-from-measurement.md)
turning into a measurement. That ADR argued from the scheduling data that "a
real scanner talking to this machine would drop its connections"; here one
does, on demand, and stops when the budget is raised above what the host's
scheduling actually costs.

Two things follow. **The default multiplier should be 2 (×16) or more**, not
CIP's minimum — 160 ms at a 10 ms RPI, comfortably above the ~50 ms of
disruption measured in ADR 0009. And **the fresh/stale ratio is set by RPI
against scan rate**, not by anything being wrong: at RPI 2 ms under a 10 ms
scan the scanner sees 486 fresh of 499, while at RPI 10 ms under a 10 ms scan
it sees roughly half, because the process image holds the last frame received
and an unsynchronised scan re-reads it.

## A measurement trap this run walked into

The first instrumented run reported **14–20 % of exchanges timing out**, which
looked like a serious performance problem against the 0.08 % measured for the
IPC path alone (ADR 0009).

It was not. Both sides reported *exactly* 101 timeouts despite running for
different durations — too neat for scheduling noise. The harness started 1 s
before its stack process, and 1 s of a 10 ms task is ~100 scans with no peer to
answer. Adding a warm-up that waits for the first successful exchange took the
figure to **0 %** and reported `warmup_scans=97`, confirming it.

Worth recording because the wrong conclusion was one step away: a timeout
figure that includes start-up says nothing about steady state, and "both
numbers are identical" is the tell.
