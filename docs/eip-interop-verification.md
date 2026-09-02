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
