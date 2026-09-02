/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_adapter_contract.c
 * @brief The rules every ProtocolAdapter must obey, checked against one.
 *
 * Written against the interface rather than the implementation on purpose:
 * when the Modbus and OPC UA adapters land, they get pointed at this same
 * function and the abstraction is either real or it isn't.
 */
#include "softplc/adapter_registry.h"
#include "softplc/plc_runtime.h"
#include "softplc/protocol_adapter.h"
#include "test_util.h"

#include <time.h>

void plc_loopback_force_timeouts(plc_protocol_adapter_t *a, unsigned n);

static void contract_lifecycle(const plc_adapter_factory_t *f) {
    plc_protocol_adapter_t *a = f->create();
    CHECK(a != NULL);
    if (!a) return;

    /* Before open(): CLOSED, and exchange() must refuse rather than touch the
     * caller's buffers. */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_CLOSED);
    uint8_t in[16] = { 0xAA }, out[16] = { 0 };
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_STATE);
    CHECK_EQ_INT(in[0], 0xAA);

    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name = "contract";
    cfg.input_bytes = cfg.output_bytes = 16;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);

    /* Caps must be self-consistent and ABI-tagged. */
    plc_adapter_caps_t caps;
    CHECK_EQ_INT(plc_adapter_get_caps(a, &caps), PLC_OK);
    CHECK_EQ_INT(caps.abi_version, PLC_ADAPTER_ABI_VERSION);
    CHECK_EQ_INT(caps.input_bytes, 16);
    CHECK_EQ_INT(caps.output_bytes, 16);
    CHECK(caps.name[0] != '\0');
    CHECK(caps.protocol[0] != '\0');
    CHECK(caps.exchange_timeout_us > 0);
    CHECK(caps.failsafe_timeout_us > 0);
    /* point_overrides is reserved in this release; an adapter claiming
     * capacity would be claiming behaviour that does not exist yet. */
    CHECK_EQ_INT(caps.point_override_capacity, 0);

    /* open() must zero the counters, not accumulate across sessions. */
    plc_adapter_stats_t stats;
    CHECK_EQ_INT(plc_adapter_get_stats(a, &stats), PLC_OK);
    CHECK_EQ_INT(stats.exchanges, 0);
    CHECK_EQ_INT(stats.timeouts, 0);
    CHECK_EQ_INT(stats.stale_for_us, 0);

    /* Oversized requests are rejected, not truncated. */
    uint8_t big[64] = { 0 };
    CHECK_EQ_INT(plc_adapter_exchange(a, big, sizeof(big), in, sizeof(in)),
                 PLC_ERR_INVAL);

    CHECK_EQ_INT(plc_adapter_close(a), PLC_OK);
    CHECK_EQ_INT(plc_adapter_close(a), PLC_OK);   /* idempotent */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_CLOSED);

    f->destroy(a);
}

/* The central guarantee: however the peer misbehaves, exchange() writes the
 * whole input image before returning - holding until the image has been stale
 * for failsafe_timeout_us, and only then applying the configured policy.
 *
 * The window is set to 3 ms and the misses are spaced 2 ms apart so the test
 * crosses it in real time. It is a duration now, so there is no count to step
 * through: what matters is that a miss inside the window holds regardless of
 * policy, and one past it does not. */
#define FAILSAFE_WINDOW_US 3000
#define MISS_SPACING_US    2000

static void sleep_us_(unsigned us) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)us * 1000L };
    nanosleep(&ts, NULL);
}

static void contract_failsafe(plc_failsafe_policy_t policy, uint8_t expect_byte) {
    const plc_adapter_factory_t *f = plc_adapter_registry_find("loopback");
    plc_protocol_adapter_t *a = f->create();

    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name = "failsafe";
    cfg.input_bytes = cfg.output_bytes = 8;
    cfg.failsafe_policy = policy;
    cfg.failsafe_timeout_us = FAILSAFE_WINDOW_US;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);

    uint8_t out[8], in[8];
    memset(out, 0x5A, sizeof(out));
    memset(in, 0, sizeof(in));

    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);
    CHECK_EQ_INT(in[0], 0x5A);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_ONLINE);

    plc_loopback_force_timeouts(a, 4);

    /* Inside the window: HOLD regardless of policy - a dropped frame must not
     * inject an edge into the process image. */
    memset(in, 0xFF, sizeof(in));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(in[0], 0x5A);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    sleep_us_(MISS_SPACING_US);
    memset(in, 0xFF, sizeof(in));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(in[0], 0x5A);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    /* Past the window: the configured policy decides. */
    sleep_us_(MISS_SPACING_US);
    memset(in, 0xFF, sizeof(in));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(in[0], expect_byte);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_FAULTED);

    plc_adapter_stats_t s;
    plc_adapter_get_stats(a, &s);
    CHECK(s.timeouts >= 3);
    CHECK(s.stale_for_us >= FAILSAFE_WINDOW_US);
    CHECK_EQ_INT(s.failsafe_activations, 1);   /* the transition, not each scan */

    /* Recovery: a good exchange makes the image fresh again. */
    plc_loopback_force_timeouts(a, 0);
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_ONLINE);
    plc_adapter_get_stats(a, &s);
    CHECK_EQ_INT(s.stale_for_us, 0);

    plc_adapter_close(a);
    f->destroy(a);
}

static void test_registry(void) {
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    CHECK(plc_adapter_registry_count() >= 1);
    CHECK(plc_adapter_registry_find("loopback") != NULL);
    CHECK(plc_adapter_registry_find("does-not-exist") == NULL);
#if SOFTPLC_WITH_EIP
    /* The core must reach the EtherNet/IP stack by protocol name only - if it
     * ever needs the concrete type, the abstraction has failed. */
    CHECK(plc_adapter_registry_find("ethernet-ip") != NULL);
#endif

    /* Duplicate registration is a configuration error, not a silent replace. */
    const plc_adapter_factory_t *lb = plc_adapter_registry_find("loopback");
    CHECK_EQ_INT(plc_adapter_registry_add(lb), PLC_ERR_STATE);
}

/* An adapter must survive being driven by the real scan engine, and the
 * engine must survive a FAULTED adapter. */
static void test_runtime_integration(void) {
    const plc_adapter_factory_t *f = plc_adapter_registry_find("loopback");
    plc_protocol_adapter_t *a = f->create();

    plc_adapter_config_t ac;
    plc_adapter_config_init(&ac);
    ac.name = "rt";
    ac.input_bytes = ac.output_bytes = 8;
    CHECK_EQ_INT(plc_adapter_open(a, &ac), PLC_OK);

    plc_runtime_config_t rc;
    plc_runtime_config_init(&rc);
    rc.cycle_us = 0;    /* free-running: no sleeping in a unit test */
    rc.i_bytes = rc.q_bytes = 16;
    plc_runtime_t *rt = plc_runtime_create(&rc);
    CHECK(rt != NULL);

    CHECK_EQ_INT(plc_runtime_bind_adapter(rt, a, 0, 0), PLC_OK);
    /* A slice that does not fit must be refused at bind time. */
    CHECK_EQ_INT(plc_runtime_bind_adapter(rt, a, 12, 0), PLC_ERR_INVAL);

    /* Outputs written in scan N appear on the inputs in scan N+1: the engine
     * transmits the previous scan's latched %Q. */
    plc_process_image_t *pi = plc_runtime_image(rt);
    plc_pi_set_byte(pi, PLC_AREA_Q, 0, 0x42);
    CHECK_EQ_INT(plc_runtime_scan_once(rt), PLC_OK);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_I, 0), 0x00);
    CHECK_EQ_INT(plc_runtime_scan_once(rt), PLC_OK);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_I, 0), 0x42);

    /* A faulted adapter is counted but does not stop the scan. */
    plc_loopback_force_timeouts(a, 5);
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ_INT(plc_runtime_scan_once(rt), PLC_ERR_TIMEOUT);
    }
    plc_runtime_stats_t rs;
    plc_runtime_get_stats(rt, &rs);
    CHECK_EQ_INT(rs.io_faults, 5);
    CHECK_EQ_INT(rs.scans, 7);

    plc_runtime_destroy(rt);
    plc_adapter_close(a);
    f->destroy(a);
}

int main(void) {
    test_registry();
    contract_lifecycle(plc_adapter_registry_find("loopback"));
    contract_failsafe(PLC_FAILSAFE_HOLD,  0x5A);
    contract_failsafe(PLC_FAILSAFE_CLEAR, 0x00);
    test_runtime_integration();
    TEST_REPORT("adapter_contract");
}
