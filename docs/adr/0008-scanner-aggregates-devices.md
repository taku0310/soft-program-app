# 0008 — The Scanner aggregates its devices into one adapter

*Status: accepted*

## Context

ADR 0007 established that OpENer cannot originate, so the Scanner role needs a
second stack. With a stack chosen (EIPScanner), one question decides the shape
of the whole thing: a scanner drives *N* remote devices, but
`plc_protocol_adapter_t` describes one image pair and reports one
`plc_adapter_state_t`.

This deployment has four devices, and keeps the Adapter role as well.

Two ways to reconcile it:

* **A — one adapter instance per device.** The interface is untouched. Each
  device gets its own `%I`/`%Q` slice, its own state and its own failsafe, all
  from machinery that already exists.
* **B — one adapter instance aggregating N devices.** One process, one shared
  memory region, one `exchange()` per scan.

At four devices, A is genuinely tenable, and it was the cheaper option to
build: no new concepts at all.

## Decision

**Option B**, with per-device health published as data.

## Consequences

The deciding argument is scan time, and it is one this project already
committed to. `docs/architecture.md` justifies doing I/O **once per scan** so
that "the worst case scan time is a sum of known budgets rather than a sum of
two". Option A breaks exactly that: four devices means four sequential
`exchange()` calls, each with its own timeout, so a worst-case scan carries
**4 × the timeout** instead of one. Choosing A would have contradicted the
reasoning the scan engine was built on, to save work in the adapter.

Two lesser arguments point the same way: four processes, four shared-memory
regions and four sets of doorbells is real operational weight for four drives;
and `PLC_MAX_BINDINGS` is 8, so A stops working entirely if the line grows.

What B costs, and how it is paid:

* **A single `plc_adapter_state_t` cannot say "device 7 is down while the rest
  are fine"** — which is precisely the condition that matters most on a line.
  Rather than widen the adapter interface for one protocol's benefit, the
  scanner applies the failsafe policy **per device**, to that device's slice
  only, under that device's own policy from the device table; and publishes a
  **health byte per device** at the head of the input image. A POU reads it as
  an ordinary input.

  That is the right home for it independently of the interface question: what
  to do when one drive drops is plant logic, not runtime policy. The runtime's
  job is to make the fact available and keep the other three devices running,
  which it does.

* **The adapter-level failsafe becomes the coarse one.** It fires only when the
  scanner *process* stops answering, and then the whole image — health bytes
  included — is held or cleared, because nothing is known about any device.
  Under CLEAR the health bytes zero to `EIP_DEVICE_OFFLINE`, which is the
  correct thing to tell a POU when the scanner is gone.

* **Configuration grows into a file.** A device table needs an address, three
  assembly instances, two sizes, two RPIs and a policy per device; that does
  not fit flat environment variables legibly. The format is line-based rather
  than JSON or YAML because this project has no third-party runtime
  dependencies and a four-row table is not worth acquiring one for. Every field
  is required — a silently defaulted RPI or image size is a plant fault waiting
  to happen.

* **Both processes read the same table**, and the scanner cross-checks its
  totals against the layout the core published. A mismatch is a start-up
  failure naming both sides, rather than a truncated image at run time.

## Note on the direction mapping

Recorded here because it is the most likely source of a bug in this area: the
CIP labels invert between the roles while `%I`/`%Q` do not. As an Adapter, `%Q`
is the produced assembly (T→O); as a Scanner, `%Q` is O→T. Anyone reasoning
from `eip_shm_layout.h` while editing `eip_scanner_shm_layout.h` will get it
backwards, and the result — outputs appearing on inputs — reaches machinery
before it reaches a log.
