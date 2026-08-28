# 0005 — Failsafe is selectable per adapter: HOLD or CLEAR

*Status: accepted*

## Context

When an adapter stops delivering fresh inputs, the core must still hand the
POUs a fully defined `%I` image. What it should contain has no universal
answer:

* A conveyor's presence sensor going to 0 stops the line. Safe.
* An interlock's "guard closed" going to 0 stops the line. Also safe.
* An emergency-stop's normally-closed contact going to 0 reads as *E-stop
  pressed*, which may be right — or a level sensor's 0 reads as *tank empty*,
  which triggers a fill into a tank that may be full.

Holding is equally wrong somewhere: a held "part present" can drive a robot
into an empty fixture.

## Decision

The policy is **selected per adapter** through `plc_adapter_config` and
reported through `AdapterCaps::failsafe_policy`:

* `PLC_FAILSAFE_HOLD` — keep the last successfully received image.
* `PLC_FAILSAFE_CLEAR` — zero the image.

There is no global default. `plc_adapter_config_init()` seeds `HOLD` as the
conservative starting point — holding cannot invent an edge that never
happened, whereas clearing can — but the choice is meant to be made explicitly
per deployment.

**Below the threshold, both policies hold.** Only once
`consecutive_timeout_threshold` is crossed does the configured policy take
effect.

## Consequences

That last rule is the substantive part of this decision. A single missed frame
is a transport hiccup, not a lost peer; zeroing the image for one scan would
inject a falling edge on *every* input, and a POU reading edges cannot tell
that from a real event. `CLEAR` is about a peer that is gone, and the threshold
is what establishes "gone".

Consequences:

* `plc_adapter_apply_failsafe()` is shared by every adapter, so HOLD and CLEAR
  mean exactly the same thing on EtherNet/IP, Modbus and OPC UA.
* `exchange()` writes the full input image even when it returns
  `PLC_ERR_TIMEOUT`. The core never sees an undefined or torn image, so callers
  may use the buffer whatever the return code — which is what lets the scan
  engine treat an I/O fault as recoverable.
* `failsafe_activations` counts the *transition* into `FAULTED`, not each
  faulted scan, so it reads as "how many times did we lose the peer".
* Both policies are covered end-to-end across a real process kill
  (`test_eip_ipc.c`, `test_eip_failsafe_clear.c`) — testing only one would
  leave a selectable policy half verified.
