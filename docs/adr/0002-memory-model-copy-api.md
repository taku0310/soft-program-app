# 0002 — Copy API over SPSC rings; PLC memory is never shared

*Status: accepted*

## Context

Adapters run out of process for fault isolation (ADR 0003). Two ways to move
the cyclic images across:

* **A — map the process image into the adapter.** Zero copies, lowest latency.
* **B — copy through a queue.** Two memcpys per direction per scan.

## Decision

**Option B.** The process image is never mapped into an adapter. Each exchange
is a pair of fixed-size frames pushed through an SPSC ring in shared memory
(`include/softplc/ipc/spsc_ring.h`), one ring per direction.

## Consequences

Option A would have made the fault isolation of ADR 0003 mostly decorative: a
wild write in a protocol stack would land directly in `%I`/`%Q`, and a process
boundary that still shares the memory being protected protects nothing that
matters. Option B makes the containment real — the worst a broken adapter can
do is fail to publish a frame.

The ring reinforces this rather than merely enabling it. The payload is written
before the cursor is published with a release store, so a producer that dies
mid-frame leaves the slot **unpublished**; the consumer sees the ring exactly as
it was, never half a frame. A full ring refuses the push instead of overwriting,
because a dropped new frame is recoverable and a clobbered unread one is not.
And the length field, which the peer writes, is validated on pop — a corrupt or
hostile peer must not be able to turn it into an out-of-bounds read.

Costs accepted:

* Two memcpys per direction per scan. At 32 bytes and a 10 ms task this is far
  below the noise floor of the scheduling jitter.
* A frame size ceiling (`PLC_IPC_MAX_FRAME_BYTES`, 496 bytes — sized to fit
  inside OpENer's 512-byte Ethernet buffer after the CIP sequence count and the
  run/idle header). Larger images need fragmentation, which is not implemented.
