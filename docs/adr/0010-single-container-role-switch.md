# 0010 — One container, one role, switched by configuration

*Status: accepted*

## Context

The deployment shipped as three containers — `plc-core`, `eip-adapter`,
`eip-scanner` — because the process split is
[the crash-containment boundary](0003-crash-containment-only.md) and each
process had an obvious container to live in.

Three is more than most deployments need. A PLC is usually *either* a device
that some other controller scans *or* an originator driving remote I/O, not
both, and a deployment that uses one role was still being handed three
services, two of which it did not want.

The obvious simplification — link the stack into the core and ship one process
— is the one thing that cannot be done. It would put OpENer's network-facing
CIP implementation inside the address space holding the process image, which is
exactly what [ADR 0002](0002-memory-model-copy-api.md) and
[ADR 0003](0003-crash-containment-only.md) exist to prevent, and it would drag
the Scanner's C++20 dependency into a C11 core.

There is also a constraint that is not ours to relax. CIP class 1 I/O uses a
**fixed UDP port, 2222, at both ends**. An Adapter and a Scanner in one network
namespace therefore receive each other's transmissions; this was not deduced
from the specification but hit during interop verification, where it appeared
as `Received data from unknown connection T2O_ID=<our own O2T id>` and was only
resolved by putting the two roles in separate network namespaces
([docs/eip-interop-verification.md](../eip-interop-verification.md)). A
container is one network namespace.

## Decision

**A `role` setting selects which single protocol stack runs beside the core,
and one image carries all of them.**

    role = none | adapter | scanner

`docker/Dockerfile.softplc` builds the core and both stacks into one image.
`docker/entrypoint.sh` reads the role back out of the core binary
(`softplc --role`), starts the core, and starts the one stack process that role
calls for.

Three things follow from that shape, and each was chosen rather than fallen
into:

* **The role resolves inside `src/adapters/builtins.c`**, the one file allowed
  to know a protocol's name. `src/core/` still has no link-time dependency on
  any protocol, and a build without the Scanner simply has no `scanner` role.
* **The core and the entrypoint read the same value from the same place.** The
  entrypoint does not parse configuration; it asks the binary. The two halves
  of a single-container deployment cannot be configured to disagree about what
  the container is.
* **The stack is still a separate process.** One container is a packaging
  decision, not a memory-sharing one.

Settings gained a file underneath the environment
([`plc_config.h`](../../include/softplc/plc_config.h)) because one container
now holds settings for two processes, and a compose `environment:` block is a
poor place to explain why a timeout is what it is. The environment still wins,
so no existing deployment changes behaviour.

## Consequences

Both EtherNet/IP roles at once still means **two network namespaces** — two
containers, or two pods. That is the honest limit, and the three-service
`docker-compose.yml` remains for it. `role` is one value, not a set, for the
same reason.

`SOFTPLC_ADAPTERS` remains as the escape hatch: it takes an arbitrary adapter
list and overrides the role's. Setting both to different things logs a warning
rather than silently picking one, because the role still decides which stack
process is started next to the core, and a core bound to `ethernet-ip` beside a
Scanner process is a PLC that scans against a peer that is not there.

Inside one container the entrypoint is the supervisor that
[ADR 0004](0004-no-restart-no-reconnect-state.md) deferred to, so it is the one
place in the project that restarts anything. A stack crash is restarted there
without the core noticing, which was verified by `SIGKILL`ing the stack process
mid-run: the core kept scanning across the gap, applied its HOLD failsafe after
109 ms, and returned to `ONLINE` when the restarted stack reattached. A core
exit is *not* recovered, and must not be — the core owns the shared memory and
the doorbells, so its exit invalidates the stack's mappings and the container
has to go down and be restarted whole.

One container also means one uid, which removes the `CAP_DAC_OVERRIDE` the
split deployment needs to let a root stack container read shared memory created
`0600` by uid 10001. The all-in-one image runs unprivileged with `cap_drop:
ALL` and no `cap_add` at all.

What this does not do is make the single-container form the default. Both are
supported, and the choice is a deployment question: one container when the PLC
has one role, three when it genuinely has two.
