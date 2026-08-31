# Architecture decision records

The decisions that shaped this rebuild, with the reasoning that produced them
and — as importantly — what each one rules out.

| # | decision | status |
|---|---|---|
| [0001](0001-iec-61131-3-core-with-61499-sifb-adapters.md) | 61131-3 core, 61499 SIFB idea for the I/O layer | accepted |
| [0002](0002-memory-model-copy-api.md) | Copy API over SPSC rings, never shared PLC memory | accepted |
| [0003](0003-crash-containment-only.md) | Crash containment only; no dedicated watchdog | accepted |
| [0004](0004-no-restart-no-reconnect-state.md) | Process restart and reconnect state are out of scope | accepted |
| [0005](0005-failsafe-policy.md) | Failsafe selectable per adapter: HOLD or CLEAR | accepted |
| [0006](0006-point-overrides-reserved.md) | `point_overrides`: type reserved, unimplemented | accepted |
| [0007](0007-opener-as-eip-stack.md) | OpENer as the EtherNet/IP stack, vendored as a submodule | accepted |
| [0008](0008-scanner-aggregates-devices.md) | The Scanner aggregates its devices into one adapter | accepted |
