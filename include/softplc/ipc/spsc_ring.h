/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file spsc_ring.h
 * @brief Single-producer / single-consumer ring for the copy-API boundary.
 *
 * This is the concrete mechanism behind memory model B.  The PLC process image
 * is never mapped into an adapter process; instead every cyclic exchange is a
 * pair of fixed-size frames pushed through two of these rings, one per
 * direction.  The cost is two memcpys per direction per scan; what it buys is
 * that an adapter fault cannot reach PLC memory - the only thing it can do is
 * fail to publish a frame.
 *
 * Lock-free by construction: the producer only ever advances @c head and the
 * consumer only ever advances @c tail, and the payload write is ordered before
 * the head store by a release.  That matters more than usual here, because the
 * peer may die at any instruction: a producer that dies mid-frame leaves the
 * slot unpublished, so the consumer sees the ring exactly as it was before the
 * write started rather than half a frame.
 *
 * Both structures live in shared memory, so they must contain no pointers and
 * no process-local state; index arithmetic is modular over uint32_t.
 */
#ifndef SOFTPLC_IPC_SPSC_RING_H
#define SOFTPLC_IPC_SPSC_RING_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest cyclic image an out-of-process adapter may carry, in bytes.
 *  Sized to stay inside OpENer's 512-byte Ethernet message buffer once the
 *  CIP sequence count and the 32-bit run/idle header are accounted for. */
#define PLC_IPC_MAX_FRAME_BYTES 496

/** Slots per ring.  Power of two, and deliberately small: this is a cyclic
 *  bus, so a backlog is staleness, not throughput.  Four gives the consumer
 *  room to be one scan late without the producer ever blocking. */
#define PLC_IPC_RING_SLOTS 4

#define PLC_IPC_CACHELINE 64

typedef struct plc_ipc_frame {
    uint32_t seq;   /**< echoed by the responder so replies can be matched */
    uint32_t len;   /**< payload length, <= PLC_IPC_MAX_FRAME_BYTES        */
    uint8_t  data[PLC_IPC_MAX_FRAME_BYTES];
} plc_ipc_frame_t;

typedef struct plc_spsc_ring {
    /* head and tail sit on separate cache lines: they are written by
     * different CPUs every cycle, and sharing a line would turn each
     * exchange into a ping-pong of exclusive-state transfers. */
    alignas(PLC_IPC_CACHELINE) _Atomic uint32_t head; /**< producer cursor */
    alignas(PLC_IPC_CACHELINE) _Atomic uint32_t tail; /**< consumer cursor */
    alignas(PLC_IPC_CACHELINE) plc_ipc_frame_t slots[PLC_IPC_RING_SLOTS];
} plc_spsc_ring_t;

/* Cross-process atomics only work if they are genuinely lock-free; a
 * lock-backed atomic would put a process-local mutex in shared memory. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "cross-process SPSC ring requires lock-free 32-bit atomics");

/** Reset to empty.  Only safe while neither side is running. */
void plc_spsc_init(plc_spsc_ring_t *r);

/**
 * @brief Publish one frame.  Producer side only.
 * @return ::PLC_OK, ::PLC_ERR_AGAIN when full, ::PLC_ERR_INVAL on a bad length.
 */
plc_status_t plc_spsc_push(plc_spsc_ring_t *r, uint32_t seq,
                           const void *data, uint32_t len);

/**
 * @brief Consume the oldest frame.  Consumer side only.
 * @param out    receives the frame; @p out->len is clamped to @p cap.
 * @return ::PLC_OK, ::PLC_ERR_AGAIN when empty, ::PLC_ERR_PROTO on a frame
 *         whose length field is impossible (a corrupt or hostile peer).
 */
plc_status_t plc_spsc_pop(plc_spsc_ring_t *r, plc_ipc_frame_t *out, uint32_t cap);

/** Frames currently queued. */
uint32_t plc_spsc_count(const plc_spsc_ring_t *r);

/** Drop every queued frame.  Consumer side only; used to resynchronise after
 *  a timeout, where anything still queued is by definition stale. */
void plc_spsc_drain(plc_spsc_ring_t *r);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_IPC_SPSC_RING_H */
