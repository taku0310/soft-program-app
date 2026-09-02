/* SPDX-License-Identifier: Apache-2.0 */
/** Ring invariants: FIFO order, back-pressure instead of overwrite, and
 *  correct behaviour across the uint32_t cursor wrap. */
#include "softplc/ipc/spsc_ring.h"
#include "test_util.h"

static plc_spsc_ring_t g_ring;   /* static: it is bigger than a stack frame wants */

static void test_empty(void) {
    plc_spsc_init(&g_ring);
    plc_ipc_frame_t f;
    CHECK_EQ_INT(plc_spsc_count(&g_ring), 0);
    CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, PLC_IPC_MAX_FRAME_BYTES), PLC_ERR_AGAIN);
}

static void test_fifo(void) {
    plc_spsc_init(&g_ring);
    const uint8_t payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

    for (uint32_t i = 0; i < PLC_IPC_RING_SLOTS; ++i) {
        CHECK_EQ_INT(plc_spsc_push(&g_ring, 100 + i, payload, sizeof(payload)), PLC_OK);
    }
    CHECK_EQ_INT(plc_spsc_count(&g_ring), PLC_IPC_RING_SLOTS);

    /* Full must refuse, never overwrite: a dropped new frame is recoverable,
     * a clobbered unread one is not. */
    CHECK_EQ_INT(plc_spsc_push(&g_ring, 999, payload, sizeof(payload)), PLC_ERR_AGAIN);

    for (uint32_t i = 0; i < PLC_IPC_RING_SLOTS; ++i) {
        plc_ipc_frame_t f;
        CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, PLC_IPC_MAX_FRAME_BYTES), PLC_OK);
        CHECK_EQ_INT(f.seq, 100 + i);
        CHECK_EQ_INT(f.len, sizeof(payload));
        CHECK_MEM_EQ(f.data, payload, sizeof(payload));
    }
    CHECK_EQ_INT(plc_spsc_count(&g_ring), 0);
}

static void test_truncation_and_limits(void) {
    plc_spsc_init(&g_ring);
    uint8_t big[PLC_IPC_MAX_FRAME_BYTES];
    memset(big, 0x5A, sizeof(big));

    CHECK_EQ_INT(plc_spsc_push(&g_ring, 1, big, sizeof(big)), PLC_OK);
    CHECK_EQ_INT(plc_spsc_push(&g_ring, 2, big, PLC_IPC_MAX_FRAME_BYTES + 1),
                 PLC_ERR_INVAL);

    plc_ipc_frame_t f;
    CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, 8), PLC_OK);
    CHECK_EQ_INT(f.len, 8);   /* clamped to the reader's capacity */
}

static void test_zero_length(void) {
    plc_spsc_init(&g_ring);
    CHECK_EQ_INT(plc_spsc_push(&g_ring, 7, NULL, 0), PLC_OK);
    plc_ipc_frame_t f;
    CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, PLC_IPC_MAX_FRAME_BYTES), PLC_OK);
    CHECK_EQ_INT(f.seq, 7);
    CHECK_EQ_INT(f.len, 0);
}

static void test_drain(void) {
    plc_spsc_init(&g_ring);
    for (uint32_t i = 0; i < PLC_IPC_RING_SLOTS; ++i) {
        plc_spsc_push(&g_ring, i, NULL, 0);
    }
    plc_spsc_drain(&g_ring);
    CHECK_EQ_INT(plc_spsc_count(&g_ring), 0);

    /* Still usable after a drain - the proxy drains before every request. */
    CHECK_EQ_INT(plc_spsc_push(&g_ring, 42, NULL, 0), PLC_OK);
    plc_ipc_frame_t f;
    CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, 4), PLC_OK);
    CHECK_EQ_INT(f.seq, 42);
}

/* The cursors are uint32_t and never reset, so on a 10 ms task they wrap after
 * about 16 months of continuous running.  Force the wrap here rather than
 * discovering it in a plant. */
static void test_cursor_wraparound(void) {
    plc_spsc_init(&g_ring);
    atomic_store(&g_ring.head, 0xFFFFFFFEu);
    atomic_store(&g_ring.tail, 0xFFFFFFFEu);

    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t v = (uint8_t)i;
        CHECK_EQ_INT(plc_spsc_push(&g_ring, 0xFFFFFFFEu + i, &v, 1), PLC_OK);
        plc_ipc_frame_t f;
        CHECK_EQ_INT(plc_spsc_pop(&g_ring, &f, 1), PLC_OK);
        CHECK_EQ_INT(f.seq, (uint32_t)(0xFFFFFFFEu + i));
        CHECK_EQ_INT(f.data[0], v);
    }
    CHECK_EQ_INT(plc_spsc_count(&g_ring), 0);
}

int main(void) {
    test_empty();
    test_fifo();
    test_truncation_and_limits();
    test_zero_length();
    test_drain();
    test_cursor_wraparound();
    TEST_REPORT("spsc_ring");
}
