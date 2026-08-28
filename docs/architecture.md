# Architecture

## What this is

A soft PLC that keeps an **IEC 61131-3** core and reaches the plant through a
pluggable protocol layer. The first protocol is EtherNet/IP, ported from
**OpENer** (EIPStackGroup) — the de facto reference implementation, with real
ODVA conformance history behind it. Modbus/TCP and OPC UA are meant to arrive
behind the same interface without the core changing.

## The two ideas it is built on

**The core stays 61131-3.** Cyclic scan, process image, standard function
blocks, tasks with periods and deadlines. That is what the competitive
comparison is against, and moving off it would forfeit the comparison.

**The I/O layer borrows from 61499 — one idea, not the model.** IEC 61499's
Service Interface Function Block is a self-describing block with a declared
capability set, an explicit lifecycle and a service call, wired in by
configuration rather than by name. That decoupling is what this project
imports, in `include/softplc/protocol_adapter.h`. It does **not** import
61499's event-driven execution model, its scheduling, or its distribution
semantics; the runtime is cyclic and stays cyclic. The SIFB idea buys exactly
one thing: EtherNet/IP, Modbus and OPC UA plug into the same socket.

```
┌──────────────────────────── PLC core process ────────────────────────────┐
│                                                                          │
│   POUs (IEC 61131-3)                                                     │
│        │  read %I / write %Q                                             │
│   ┌────▼──────────────┐    ┌───────────────────┐                         │
│   │  process image    │◄──►│   scan engine     │  cyclic task, deadlines │
│   │  %I  %Q  %M       │    │  in → exec → out  │                         │
│   └───────────────────┘    └─────────┬─────────┘                         │
│                                      │ exchange() once per scan          │
│                            ┌─────────▼─────────┐                         │
│                            │ ProtocolAdapter   │  ← the SIFB boundary    │
│                            └─────────┬─────────┘                         │
│                    ┌─────────────────┼──────────────────┐                │
│              ┌─────▼─────┐    ┌──────▼──────┐    ┌──────▼──────┐         │
│              │ loopback  │    │ EtherNet/IP │    │ Modbus, OPC │         │
│              │ (in-proc) │    │   proxy     │    │ UA (future) │         │
│              └───────────┘    └──────┬──────┘    └─────────────┘         │
└──────────────────────────────────────┼───────────────────────────────────┘
                                       │  shared memory: 2 × SPSC ring
                     ═══════════════════════════════════  crash boundary
┌──────────────────────────────────────┼───────────────────────────────────┐
│                            ┌─────────▼─────────┐                         │
│                            │  IPC service loop │                         │
│                            └─────────┬─────────┘                         │
│                            ┌─────────▼─────────┐                         │
│                            │  OpENer / CIP     │  ← may crash            │
│                            └─────────┬─────────┘                         │
└──────────────────────────────────────┼───────────────────────────────────┘
                                  EtherNet/IP scanner
```

## The scan

```
1. INPUT    for each binding: adapter->exchange(latched %Q, %I slice)
2. EXECUTE  each POU in configured order
3. OUTPUT   %Q is latched into the shadow the next scan will transmit
```

Outputs computed in scan *N* reach the field at the top of scan *N+1*. That is
one scan of deliberate latency, bought for a real property: adapters are asked
for I/O exactly **once** per cycle, so the worst-case scan time is a sum of
known budgets rather than of two blocking points.

## Memory model: copy, never share

The process image is never mapped into an adapter. Every exchange is a pair of
fixed-size frames copied through an SPSC ring in shared memory.

Two memcpys per direction per scan is the price. What it buys is that a fault
in a protocol stack **cannot reach PLC memory**. The worst an adapter can do is
fail to publish a frame — and the ring is written payload-first with a release
store on the cursor, so a producer that dies mid-frame leaves the slot
unpublished rather than half-written.

See [ADR 0002](adr/0002-memory-model-copy-api.md).

## Failure model: containment, not recovery

The adapter runs in its own process so that a stack fault is survivable. That
is the entire goal. Specifically **not** goals:

* **No dedicated watchdog.** `exchange()` has a deadline and it is the liveness
  check. A peer that cannot answer in time is, to a control loop,
  indistinguishable from a dead one — and a second liveness channel would only
  create a way for the two signals to disagree.
* **No process restart.** The core degrades to failsafe and keeps scanning; a
  supervisor or the container runtime brings the adapter back.
* **No state reconciliation on reconnect.** A returning adapter starts from
  zeroed assemblies and the first fresh exchange overwrites the failsafe image.
  Recovering plant state across a reconnect is application logic.

See [ADR 0003](adr/0003-crash-containment-only.md) and
[ADR 0004](adr/0004-no-restart-no-reconnect-state.md).

## Failsafe

Selectable per adapter through `AdapterCaps`, because neither answer is right
everywhere:

* `PLC_FAILSAFE_HOLD` — keep the last good image.
* `PLC_FAILSAFE_CLEAR` — zero it.

One rule applies under both: **below the threshold, always hold.** A single
missed frame is a transport hiccup, and zeroing the image for one scan would
inject a falling edge on every input that a POU would read as a real event.
`CLEAR` is about a peer that is *gone*, and the consecutive-timeout threshold
is what decides that.

See [ADR 0005](adr/0005-failsafe-policy.md).

## Direction mapping

OpENer implements an EtherNet/IP **adapter** (a CIP target), so an external
scanner owns the connection. That fixes the mapping, once, in
`eip_shm_layout.h`:

| PLC | ring | assembly | wire |
|---|---|---|---|
| `%Q` outputs | `req` | produced | T→O |
| `%I` inputs | `rsp` | consumed | O→T |

## Layout

```
include/softplc/
  protocol_adapter.h    the SIFB-inspired interface — start here
  adapter_registry.h    factories, keyed by protocol name
  plc_runtime.h         scan engine
  process_image.h       %I / %Q / %M
  std_fb.h              TON, TOF, TP, R_TRIG, CTU, ...
  ipc/spsc_ring.h       the copy-API mechanism
  ipc/shm.h             shared memory and doorbells

src/core/               61131-3 runtime; names no protocol
src/adapters/
  builtins.c            the only file that knows which stacks exist
  loopback/             in-process reference adapter
  eip/
    eip_shm_layout.h    the cross-process contract
    eip_adapter_proxy.c core side  — timeouts, failsafe, sequence matching
    eip_adapter_main.c  adapter side — the IPC service loop
    eip_backend.h       "an EtherNet/IP stack", abstracted
    eip_backend_opener.c   the real thing
    eip_backend_loopback.c mirror, for building without the submodule
```

`src/core/` has no link-time dependency on any protocol — deliberately. If it
named `plc_eip_adapter_factory`, the pluggability claim would be false at the
link level however clean the headers looked.

## Adding a protocol

1. Implement `plc_adapter_vtbl_t`.
2. Export a `plc_adapter_factory_t`.
3. Add an `extern` to `src/adapters/builtins.c` and a source to CMake.
4. Point `tests/test_adapter_contract.c` at it.

Nothing in `src/core/` changes. For an out-of-process protocol, `src/ipc/` and
the proxy/service-loop split in `src/adapters/eip/` are meant to be copied —
the crash-containment behaviour comes with them.

## Known gaps

* **`consecutive_timeout_threshold` default is provisional.** 3 is a
  placeholder, not a measurement. Phase 0 golden data needs to report the
  observed round-trip distribution under load first. It is runtime-configurable
  so tuning does not need a rebuild.
* **`point_overrides` is type-only.** The struct is fixed so the ABI will not
  have to change when forcing/simulation lands; no adapter implements it and
  every one reports `point_override_capacity == 0`.
* **No ST/IL/LD front end.** POUs are C against the runtime API. The runtime
  model is 61131-3; the language front end is a separate phase.
* **Modbus/TCP and OPC UA adapters are not written.** The EtherNet/IP pair is
  intended as their template.
