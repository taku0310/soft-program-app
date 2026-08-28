# 0003 — Crash containment only; no dedicated watchdog

*Status: accepted*

## Context

The EtherNet/IP stack is the largest, most network-exposed and least trusted
code in the system. If it faults, the PLC must not.

The usual follow-on question is how the core learns that a peer has died, and
the usual answer is a watchdog: a heartbeat channel, a liveness counter, a
supervising thread.

## Decision

**Process isolation for crash containment, and nothing else.** No dedicated
watchdog channel, no heartbeat thread, no liveness counter used for control.

`exchange()`'s timeout **is** the liveness check.

## Consequences

The reasoning is that to a control loop, a peer that cannot answer within the
scan budget and a peer that is dead are the *same condition* — both mean the
input image is not fresh, and both call for the same failsafe response. A
second liveness signal would add no information the timeout does not already
carry, and would add a real failure mode: the two signals disagreeing, with a
heartbeat that says "alive" while exchanges time out, or the reverse.

Consequences:

* The proxy is simpler than it would otherwise be: one deadline, one escalation
  path, one place where failsafe is applied.
* A slow peer and a dead peer are handled identically. That is intended, not a
  limitation.
* Detection latency is bounded by
  `exchange_timeout_us × consecutive_timeout_threshold` — 20 ms at the current
  provisional defaults.
* The adapter *does* publish `eip_adapter_status_t` into shared memory, but it
  is observability only. A dead process cannot update it, which is precisely
  why it must never be used for a control decision.

`test_eip_ipc.c` verifies this against a real `SIGKILL` of a real adapter
process rather than a mock, because a mocked peer cannot demonstrate the
property being claimed.
