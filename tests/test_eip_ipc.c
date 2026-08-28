/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_eip_ipc.c
 * @brief End-to-end test across the crash-containment boundary.
 *
 * Runs the real adapter binary in a real child process and drives it through
 * the real proxy: shared memory, doorbells, sequence matching and all.  Then
 * it SIGKILLs the child, which is the thing the whole out-of-process design
 * exists for - a stack fault must degrade the PLC to failsafe and nothing
 * worse.  A test that mocked the peer could not show that.
 *
 * What is under test is the IPC contract, not CIP.  With the adapter built on
 * its mirror backend (SOFTPLC_WITH_OPENER=OFF, what CI runs) the payload comes
 * back and can be compared byte for byte.  With the real stack linked the
 * inputs come from the consumed assembly instead, which is zero until a
 * scanner connects - so the payload checks are gated on the backend while
 * every transport-level assertion runs either way.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "eip_shm_layout.h"
#include "softplc/adapter_registry.h"
#include "softplc/protocol_adapter.h"
#include "test_util.h"

#ifndef EIP_ADAPTER_PATH
#  error "EIP_ADAPTER_PATH must name the adapter binary"
#endif

#define IMAGE_BYTES 16

static void sleep_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static pid_t spawn_adapter(const char *instance) {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(EIP_ADAPTER_PATH, "softplc-eip-adapter", instance, "lo", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/** Exchange until one succeeds or @p attempts run out - the adapter process
 *  needs a moment to attach, and the proxy reports that as timeouts. */
static int exchange_until_online(plc_protocol_adapter_t *a,
                                 const uint8_t *out, uint8_t *in, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (plc_adapter_exchange(a, out, IMAGE_BYTES, in, IMAGE_BYTES) == PLC_OK) {
            return 1;
        }
        sleep_ms(20);
    }
    return 0;
}

int main(void) {
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    CHECK(f != NULL);
    if (!f) TEST_REPORT("eip_ipc");

    /* Namespaced per run so parallel ctest jobs cannot collide in /dev/shm. */
    char instance[EIP_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "test%d", (int)getpid());

    plc_protocol_adapter_t *a = f->create();
    CHECK(a != NULL);

    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name        = "eip-test";
    cfg.endpoint    = instance;
    cfg.input_bytes = cfg.output_bytes = IMAGE_BYTES;
    cfg.failsafe_policy = PLC_FAILSAFE_HOLD;
    cfg.exchange_timeout_us = 200000;          /* generous: CI is slow */
    cfg.consecutive_timeout_threshold = 3;

    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);

    plc_adapter_caps_t caps;
    plc_adapter_get_caps(a, &caps);
    CHECK_EQ_INT(caps.abi_version, PLC_ADAPTER_ABI_VERSION);
    CHECK_EQ_INT(caps.flags & PLC_ADAPTER_CAP_OUT_OF_PROCESS,
                 PLC_ADAPTER_CAP_OUT_OF_PROCESS);
    /* The core creates the region, so it is OPENING until a peer answers. */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_OPENING);

    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0, sizeof(out));
    memset(in, 0xFF, sizeof(in));

    /* No peer yet: exchange must still return, on time, with a defined image. */
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);

    const pid_t child = spawn_adapter(instance);
    CHECK(child > 0);

    for (unsigned i = 0; i < sizeof(out); ++i) out[i] = (uint8_t)(i + 1);
    CHECK(exchange_until_online(a, out, in, 200));
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_ONLINE);

    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);
#if SOFTPLC_EIP_MIRROR_BACKEND
    /* The mirror backend echoes outputs into inputs, so a full round trip
     * through both rings is observable as the payload coming back. */
    CHECK_MEM_EQ(in, out, sizeof(out));
#endif

    /* Sustained exchange: every reply must be matched to its own request,
     * which is what the sequence numbers are for. */
    for (int scan = 0; scan < 50; ++scan) {
        memset(out, (uint8_t)scan, sizeof(out));
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_OK);
#if SOFTPLC_EIP_MIRROR_BACKEND
        CHECK_MEM_EQ(in, out, sizeof(out));
#endif
    }

    plc_adapter_stats_t s;
    plc_adapter_get_stats(a, &s);
    CHECK(s.exchanges >= 51);
    CHECK_EQ_INT(s.protocol_errors, 0);
    CHECK_EQ_INT(s.consecutive_timeouts, 0);

    /* --- crash containment ---------------------------------------------- */

    /* Whatever the backend last delivered is what HOLD must reproduce. */
    const uint8_t last_good = in[0];

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(out, 0xAB, sizeof(out));

    /* Misses 1 and 2: DEGRADED, image held. */
    for (int i = 0; i < 2; ++i) {
        memset(in, 0x11, sizeof(in));
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_ERR_TIMEOUT);
        CHECK_EQ_INT(in[0], last_good);
    }
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    /* Miss 3: threshold crossed, HOLD policy applied, and still held. */
    memset(in, 0x11, sizeof(in));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_FAULTED);
    CHECK_EQ_INT(in[0], last_good);

    plc_adapter_get_stats(a, &s);
    CHECK_EQ_INT(s.failsafe_activations, 1);
    CHECK(s.timeouts >= 3);

    /* The proxy must stay usable after the peer's death - no wedged
     * semaphore, no exhausted ring, no growing latency. */
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_ERR_TIMEOUT);
    }

    CHECK_EQ_INT(plc_adapter_close(a), PLC_OK);
    f->destroy(a);

    TEST_REPORT("eip_ipc");
}
