# 0004 — Process restart and reconnect state reconciliation are out of scope

*Status: accepted*

## Context

Once the adapter can die (ADR 0003), two questions follow: who restarts it, and
what happens to plant state when it comes back?

## Decision

Both are **out of scope for the PLC core**.

* **Restart** belongs to the supervisor or the container orchestrator —
  `restart: unless-stopped` in compose, a Deployment's `restartPolicy` in
  Kubernetes, a unit's `Restart=` under systemd.
* **Reconnect state reconciliation** is not attempted. A returning adapter
  starts from zeroed assemblies, and the first fresh exchange overwrites the
  failsafe image.

## Consequences

Restart supervision is a solved problem that every deployment target already
solves, usually better than an in-process implementation would, and with
policies — backoff, restart limits, escalation — a PLC core has no business
duplicating. Reimplementing it inside the core would mean a fork/exec path, a
reaping path and a backoff policy in the process that must stay the most
reliable thing in the system.

State reconciliation is deliberately left undone rather than done badly. Doing
it properly requires knowing which outputs are safe to reassert, and that is
plant knowledge the runtime does not have. A wrong answer here reasserts a
stale output onto live machinery.

Consequences:

* The core needs no `fork`, no `SIGCHLD` handler, no backoff state machine.
* The adapter exits non-zero on a fatal error rather than retrying, so a
  supervisor sees a clean signal.
* Deployments **must** configure a restart policy; nothing in this repository
  will notice that an adapter has stayed dead.
* Applications that need state recovered across a reconnect must implement it
  in POU logic, where the plant semantics are known. `plc_adapter_state()` and
  the adapter statistics expose everything needed to detect the transition.
