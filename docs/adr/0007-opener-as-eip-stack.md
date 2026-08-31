# 0007 — OpENer as the EtherNet/IP stack, vendored as a submodule

*Status: accepted*

## Context

The EtherNet/IP stack had to be ported rather than written. It needs CIP object
model, connection manager, encapsulation, assemblies and enough conformance
history to be credible.

## Decision

**OpENer** (EIPStackGroup), pinned as a git submodule at
`third_party/OpENer`, built from source by `cmake/OpENer.cmake`.

Build it with **our own CMake**, not `add_subdirectory()`.

## Consequences

OpENer is the de facto reference implementation, with ODVA conformance history
behind it, and it is a CIP *target* stack — which is what a soft PLC exposing
itself to a scanner needs. That also fixes the direction mapping once and for
all: PLC outputs are the produced (T→O) assembly, PLC inputs are the consumed
(O→T) assembly.

The build decision needs its own justification. OpENer's project builds an
`OpENer` executable whose `SAMPLE_APP` library defines
`ApplicationInitialization()`, `AfterAssemblyDataReceived()`,
`BeforeAssemblyDataSend()` and the rest — exactly the symbols our adapter must
define. Linking both is a duplicate-symbol conflict, and every workaround means
patching the submodule. Compiling the stack sources directly is the smaller and
more stable coupling: we own the application layer, upstream owns everything
below it, and updating is a submodule pointer bump rather than a merge. The
cost is that `cmake/OpENer.cmake` restates upstream's source list and its POSIX
build flags (`RESTRICT`, `_POSIX_C_SOURCE`, the run/idle header), and must be
revisited when the submodule moves.

Threading is the other consequence. OpENer is single-threaded and not
reentrant, so it gets a thread of its own and **no OpENer function is ever
called from the IPC thread**. The only shared state is the pair of assembly
byte arrays we allocated and passed to `CreateAssemblyObject()`, and even those
are not written directly: the IPC thread stages into separate buffers, and
OpENer's own callbacks (`BeforeAssemblyDataSend`, `AfterAssemblyDataReceived`)
move data across under one mutex, at the moments the stack itself guarantees no
transmit is in flight.

## Scanner (originator) role

OpENer is **adapter-only**, and this decision therefore also decides — by
omission — that the soft PLC does not scan. That consequence is large enough to
state on its own rather than leave implied by the direction mapping.

Verified against the vendored source, not assumed:

* upstream's own README opens with "OpENer is an EtherNet/IP stack for **I/O
  adapter devices**";
* `ForwardOpen()` and `LargeForwardOpen()` in `cipconnectionmanager.c` are
  request *handlers* — they take the receiving `CipInstance` and build a reply
  through `AssembleForwardOpenResponse()`. Nothing ever emits a ForwardOpen;
* there is no outbound `connect()` anywhere in `source/src`. An originator has
  to open TCP sessions to its adapters, so this alone settles it.

So the soft PLC is the device a scanner connects *to*. It cannot poll remote
I/O drops, VFDs or other adapters — which is the opposite of what a
line-controlling PLC normally does over EtherNet/IP, and is the single biggest
thing to weigh before adopting this stack.

Adding it later is a **new adapter, not a flag**: an originator stack (a
library such as EIPScanner, or a commercial one) behind a second
`plc_adapter_factory_t`, most likely registered as `"ethernet-ip-scanner"`.
The protocol adapter interface already accommodates it — a scanner's cyclic
exchange has the same shape — so the work is confined to a new adapter pair
plus its process, exactly as ADR 0001 intends. Nothing in `src/core/` changes.

Finally, `SOFTPLC_WITH_OPENER` defaults **OFF**, with a mirror backend behind
the same `eip_backend.h` interface. The tree builds and the full suite runs
without the submodule checked out, which keeps CI and a fresh clone working;
`docker/Dockerfile.eip-adapter` checks for the submodule explicitly and fails
loudly rather than shipping an image that silently serves nothing.
