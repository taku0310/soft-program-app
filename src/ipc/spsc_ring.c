/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/ipc/spsc_ring.h"

#include <string.h>

void plc_spsc_init(plc_spsc_ring_t *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

plc_status_t plc_spsc_push(plc_spsc_ring_t *r, uint32_t seq,
                           const void *data, uint32_t len) {
    if (!r || len > PLC_IPC_MAX_FRAME_BYTES || (len && !data)) return PLC_ERR_INVAL;

    const uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    /* Modular subtraction, so the cursors may wrap uint32_t freely. */
    if ((uint32_t)(head - tail) >= PLC_IPC_RING_SLOTS) return PLC_ERR_AGAIN;

    plc_ipc_frame_t *slot = &r->slots[head & (PLC_IPC_RING_SLOTS - 1)];
    slot->seq = seq;
    slot->len = len;
    if (len) memcpy(slot->data, data, len);

    /* Release: the payload above must be visible to the consumer before it can
     * observe the new head.  This is the whole guarantee the ring rests on. */
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return PLC_OK;
}

plc_status_t plc_spsc_pop(plc_spsc_ring_t *r, plc_ipc_frame_t *out, uint32_t cap) {
    if (!r || !out || cap > PLC_IPC_MAX_FRAME_BYTES) return PLC_ERR_INVAL;

    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) return PLC_ERR_AGAIN;

    const plc_ipc_frame_t *slot = &r->slots[tail & (PLC_IPC_RING_SLOTS - 1)];
    const uint32_t seq = slot->seq;
    const uint32_t len = slot->len;

    /* The peer writes this field; treat it as untrusted.  A crashed or
     * mis-built peer must not be able to turn a length field into an
     * out-of-bounds read of our own address space. */
    if (len > PLC_IPC_MAX_FRAME_BYTES) {
        atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
        return PLC_ERR_PROTO;
    }

    out->seq = seq;
    out->len = (len < cap) ? len : cap;
    if (out->len) memcpy(out->data, slot->data, out->len);

    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return PLC_OK;
}

uint32_t plc_spsc_count(const plc_spsc_ring_t *r) {
    if (!r) return 0;
    const uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (uint32_t)(head - tail);
}

void plc_spsc_drain(plc_spsc_ring_t *r) {
    if (!r) return;
    const uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, head, memory_order_release);
}
