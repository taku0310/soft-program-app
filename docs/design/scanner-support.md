# EtherNet/IP Scanner (originator) support — investigation

*Status: **implemented**. This was the investigation that preceded the work;
the decisions it fed into are recorded in
[ADR 0008](../adr/0008-scanner-aggregates-devices.md), and the code lives in
`src/adapters/eip_scanner/`. Kept as written, because the reasoning behind a
rejected option is worth as much as the chosen one.*

*Outcome: EIPScanner was adopted, and **option B** (aggregate N devices into
one adapter) was chosen over option A — see ADR 0008 for why the scan-time
argument settled it even at four devices.*

## The gap

The runtime today is an EtherNet/IP **Adapter** only: it is the device a
scanner connects to. It cannot originate connections, so it cannot drive
remote I/O drops, VFDs or other adapters — which is what a line-controlling
PLC usually does with EtherNet/IP.

This is a property of the stack, not of our wiring. Verified against the
vendored source rather than assumed:

* upstream's README opens with "OpENer is an EtherNet/IP stack for **I/O
  adapter devices**";
* `ForwardOpen()` / `LargeForwardOpen()` in `cipconnectionmanager.c` are
  request *handlers* that build a reply through
  `AssembleForwardOpenResponse()`. Nothing emits a ForwardOpen;
* there is no outbound `connect()` anywhere in `source/src`. An originator must
  open TCP sessions to its adapters, so this alone settles it.

Scanner support is therefore a **second adapter behind a second stack**, not a
flag on the existing one.

## Candidate stacks

| option | licence | language | verdict |
|---|---|---|---|
| **EIPScanner** (nimbuscontrols) | MIT | C++20 | **recommended** — see below |
| Write the originator on OpENer's CIP encoding | — | C | rejected: reimplements connection manager, session handling and Class 1 transport. Months, and the conformance risk lands on us. |
| Commercial stack (HMS, RTA, Pyramid) | proprietary | C | viable if ODVA conformance for the *scanner* role is contractually required; cost and licence terms not investigated. |

### EIPScanner — what was verified

Cloned at `12c89a5` and **built in this environment**, not just read:

* **MIT licence.** Permissive; no copyleft obligation on our binaries.
* **Builds clean** with CMake, producing `libEIPScannerS.a` (static) and a
  shared library. The `implicit_messaging` example builds too.
* **No external dependencies.** ~4,200 lines of C++ across 94 files — small
  enough to audit and to carry as a submodule the way OpENer already is.
* **It is genuinely the originator side.** `ConnectionManager` exposes
  `forwardOpen()` / `largeForwardOpen()` / `forwardClose()`, and
  `IOConnection` has `setDataToSend()` for O→T with a
  `setReceiveDataListener()` callback for T→O — Class 1 cyclic I/O, which is
  what scanning means.
* **Single-threaded and caller-driven.** `handleConnections(timeout)` runs a
  `select` and returns; the library spawns no threads of its own. That is
  *easier* to host than OpENer, which needs its own thread.
* Last commit 2024-11. Maintained, but not busy — worth weighing.

Concerns worth stating: C++20 raises the toolchain floor, and the project is
essentially one maintainer. The C++ dependency is contained by our existing
process split (below), and the small size makes a fork survivable, but neither
concern disappears.

## How it fits the existing architecture

Well, and for a reason worth noting: **the C++20 dependency never reaches the
PLC core.** It lives entirely inside a separate adapter process, behind the
same shared-memory copy API. The core stays C11 and stays unaware. This is the
crash-containment split (ADR 0003) paying off a second time, for a dependency
boundary rather than a fault boundary.

The shape mirrors the existing EtherNet/IP pair exactly:

```
softplc (C11)                      softplc-eip-scanner (C++20 + EIPScanner)
  eip_scanner_proxy.c   ◄── shm ──►   scanner service loop
  (a plc_adapter_factory_t,            ConnectionManager
   protocol "ethernet-ip-scanner")     N × IOConnection ──▶ remote devices
```

### Direction mapping — the thing most likely to cause a bug

The CIP labels **swap** between the two roles, while `%I`/`%Q` do not:

| role | PLC `%Q` goes out as | PLC `%I` comes in as |
|---|---|---|
| Adapter (today) | produced assembly, **T→O** | consumed assembly, **O→T** |
| Scanner (proposed) | **O→T** | **T→O** |

Both are "outputs out, inputs in" from the PLC's side; only the wire-direction
names invert, because in one case we are the target and in the other the
originator. Anyone reasoning from the existing `eip_shm_layout.h` comments
while writing the scanner will get this backwards. It needs stating once, in
the scanner's own layout header, and a test that pins it.

### The real design question: one device or many

A scanner talks to *N* remote devices, each with its own IP, assembly
instances, sizes, RPI and timeout multiplier. The current
`plc_protocol_adapter_t` describes one image pair and reports one
`plc_adapter_state_t`. Two ways to reconcile that:

**A — one adapter instance per device.** Interface unchanged, per-device
failsafe and state come free. But `PLC_MAX_BINDINGS` is 8, and a process and a
shared-memory region per device does not scale to a 50-drop line.

**B — one adapter instance aggregating N devices** (recommended). One process,
one region, one `exchange()` per scan; the devices' images are concatenated
into the adapter's aggregate image. The cost is that a single
`plc_adapter_state_t` cannot express "device 7 is down while the rest are
fine", which is exactly the condition that matters most.

The fix keeps the interface intact: apply the failsafe policy **per device,
inside the scanner adapter**, and surface per-device connection health as a
**status word in the input image** the POU can read. Per-device health becomes
process data rather than adapter state — which is where it belongs anyway,
since what to do when one drive drops is plant logic, not runtime policy.

### Configuration

This is the other real difference. The adapter role needs a handful of env
vars; a scanner needs a *device table* — per device: IP, config/O2T/T2O
assembly instances, both sizes, both RPIs, connection type, timeout
multiplier, plus its offset in the aggregate image. That does not fit flat
environment variables and wants a mounted config file (YAML or JSON), which
the project does not currently have or need. Adding one is part of this work,
not a detail.

## Effort

Rough, and stated as estimates rather than commitments:

| piece | estimate |
|---|---|
| Submodule, CMake, C++20 target for the scanner process | small |
| `eip_scanner_shm_layout.h` + proxy (mostly mirrors the existing pair) | small |
| Scanner service loop over `ConnectionManager` / `IOConnection` | medium |
| Device-table config format, parsing, validation | medium |
| Per-device failsafe + health-word design and tests | medium |
| Two-process tests incl. device-loss (needs a fake adapter to scan) | medium |

The last row is the one most likely to be underestimated: testing a scanner
needs something to scan. OpENer — which this repo already vendors — can serve
as that target, which is a genuine advantage of having both stacks in tree.

## Open questions for the owner

1. How many remote devices, realistically? Under ~4 makes option A tenable.
2. Is ODVA conformance required for the **scanner** role, or only for the
   adapter role already implemented? Conformance pushes toward a commercial
   stack.
3. Does the adapter role stay? Running both simultaneously is supported by the
   design but doubles the deployed processes.
4. Is a C++20 toolchain acceptable in the target build and deployment
   environment?
