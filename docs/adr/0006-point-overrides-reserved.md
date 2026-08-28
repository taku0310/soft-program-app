# 0006 — `point_overrides`: type reserved, behaviour unimplemented

*Status: accepted*

## Context

Forcing individual I/O points — for commissioning, for simulation, for
maintenance — is standard on commercial PLCs and will be wanted here. It is not
needed for this phase, and implementing it properly means answering questions
about persistence, authorisation and how a forced point is annunciated, none of
which are settled.

## Decision

Fix the **type** now (`plc_point_override_t` in `protocol_adapter.h`) and
implement nothing. Every adapter reports `point_override_capacity == 0`; the
core never populates a table.

## Consequences

The reason to define the struct before it is used is ABI. `plc_adapter_caps_t`
is the contract between the core and every adapter, and adding a field to it
later would break every compiled adapter and force a
`PLC_ADAPTER_ABI_VERSION` bump. Reserving the space now makes forcing an
additive change.

Consequences:

* The field exists in the ABI and costs a few bytes per adapter.
* `test_adapter_contract.c` asserts `point_override_capacity == 0`, so an
  adapter cannot quietly start claiming a capability that has no implementation
  behind it.
* Adding real behaviour later needs a documented decision about override
  precedence and annunciation, and `PLC_ADAPTER_CAP_POINT_OVERRIDES` exists to
  advertise it — but turning it on without bumping the ABI version would be a
  mistake, since the flag's meaning would change from "reserved" to
  "implemented".
