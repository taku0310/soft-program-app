# 0001 — IEC 61131-3 core, with the 61499 SIFB idea for the I/O layer

*Status: accepted*

## Context

The core has to stay comparable to the products this is measured against, all
of which are IEC 61131-3. At the same time the point of the project is that
EtherNet/IP, Modbus/TCP and OPC UA should be interchangeable and independently
deployable, and 61131-3 says essentially nothing useful about how a protocol
stack attaches.

IEC 61499 does address that, through the Service Interface Function Block: a
self-describing block with a declared capability set, an explicit lifecycle and
a service call, wired in by configuration rather than by name.

## Decision

Keep the core strictly 61131-3 — cyclic scan, process image, standard function
blocks, tasks with periods and deadlines.

Import from 61499 **only the SIFB decoupling idea**, as
`include/softplc/protocol_adapter.h`: an adapter declares itself through
`AdapterCaps`, has `open`/`close`/`exchange`, and is reached by protocol name
through a registry.

Explicitly do **not** import 61499's execution model, event-driven scheduling,
or distribution semantics. Scheduling stays cyclic.

## Consequences

The competitive comparison holds, and adding a protocol touches no core file.

`src/core/` is kept free of any link-time dependency on a protocol — the
factories live in `src/adapters/builtins.c` instead. Without that split the
pluggability would be a header-level fiction while the linker told the truth.

The cost is that a 61499 tool cannot drive this runtime; we took the idea, not
the standard. That was the intent.
