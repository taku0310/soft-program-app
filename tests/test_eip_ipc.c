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

static int exchange_until_answering(plc_protocol_adapter_t *a,
                                    const uint8_t *out, uint8_t *in, int attempts);

/**
 * A stopped PLC must stop its outputs.
 *
 * Measured before this was enforced: after a clean SIGTERM to the core, the
 * adapter process kept the CIP connection open and produced the last output
 * image for as long as it was left alive - 493 identical frames over five
 * seconds - so the controller saw a healthy Exclusive Owner connection
 * delivering constant data and could not tell the PLC had stopped. Its own
 * connection timeout never fired, because frames kept arriving.
 *
 * Here the core simply stops asking. The adapter has to notice and go, which
 * is what lets the far end's connection timeout fire and its failsafe apply.
 */
static void test_adapter_exits_when_the_core_stops(void) {
    char instance[64];
    snprintf(instance, sizeof(instance), "coregone-%d", (int)getpid());

    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    CHECK(f != NULL);
    if (!f) return;

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name         = "eip";
    cfg.endpoint     = instance;
    cfg.input_bytes  = IMAGE_BYTES;
    cfg.output_bytes = IMAGE_BYTES;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);

    /* Shortened so the test does not sit through the production default. */
    setenv("SOFTPLC_EIP_CORE_TIMEOUT_US", "700000", 1);
    const pid_t child = spawn_adapter(instance);
    unsetenv("SOFTPLC_EIP_CORE_TIMEOUT_US");
    CHECK(child > 0);

    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0x5A, sizeof(out));
    memset(in, 0, sizeof(in));
    CHECK(exchange_until_answering(a, out, in, 200));

    /* From here the core asks for nothing, as a stopped PLC would. */
    int exited = 0;
    for (int i = 0; i < 60 && !exited; ++i) {   /* up to 6 s */
        int status = 0;
        exited = (waitpid(child, &status, WNOHANG) == child);
        if (!exited) sleep_ms(100);
    }
    CHECK(exited);
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }

    plc_adapter_close(a);
    f->destroy(a);
}

/**
 * Exchange until the adapter process is answering, or @p attempts run out.
 *
 * "Answering" is not the same as "delivering usable data", and the difference
 * is the whole point of the two builds. Against the mirror there is a peer
 * with data, so a successful exchange is the signal. Against real OpENer with
 * no scanner attached there is no controller driving us, so every exchange is
 * correctly refused with PLC_ERR_AGAIN - and refusing *is* the evidence that
 * the adapter process is alive and serving, which is the precondition this
 * test actually needs before it kills it.
 */
static int exchange_until_answering(plc_protocol_adapter_t *a,
                                    const uint8_t *out, uint8_t *in, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        const plc_status_t st =
            plc_adapter_exchange(a, out, IMAGE_BYTES, in, IMAGE_BYTES);
#if SOFTPLC_EIP_MIRROR_BACKEND
        if (st == PLC_OK) return 1;
#else
        if (st == PLC_ERR_AGAIN) return 1;
#endif
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
    /* Each miss burns the full 200 ms budget, so 300 ms is crossed by the
     * second one - the equivalent of the old "threshold 3", expressed as the
     * duration it always really was. */
    cfg.failsafe_timeout_us = 300000;

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
    CHECK(exchange_until_answering(a, out, in, 200));

#if SOFTPLC_EIP_MIRROR_BACKEND
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_ONLINE);
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);
    /* The mirror backend echoes outputs into inputs, so a full round trip
     * through both rings is observable as the payload coming back. */
    CHECK_MEM_EQ(in, out, sizeof(out));
#else
    /* Real stack, no scanner: answering, and correctly refusing to call the
     * empty assembly valid input. */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);
#endif

    /* Sustained exchange: every reply must be matched to its own request,
     * which is what the sequence numbers are for. */
    for (int scan = 0; scan < 50; ++scan) {
        memset(out, (uint8_t)scan, sizeof(out));
        const plc_status_t st =
            plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in));
#if SOFTPLC_EIP_MIRROR_BACKEND
        CHECK_EQ_INT(st, PLC_OK);
        CHECK_MEM_EQ(in, out, sizeof(out));
#else
        CHECK_EQ_INT(st, PLC_ERR_AGAIN);
#endif
    }

    plc_adapter_stats_t s;
    plc_adapter_get_stats(a, &s);
    /* Round trips completed, whether or not the payload was usable. */
    CHECK(s.exchanges + s.data_invalid >= 51);
    CHECK_EQ_INT(s.protocol_errors, 0);
    CHECK_EQ_INT(s.stale_for_us, 0);

    /* --- crash containment ---------------------------------------------- */

    /* Whatever the backend last delivered is what HOLD must reproduce. */
    const uint8_t last_good = in[0];

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(out, 0xAB, sizeof(out));

    /* First miss: inside the window, so DEGRADED with the image held. */
    memset(in, 0x11, sizeof(in));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(in[0], last_good);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    /* Keep missing until the window is past. Bounded so a hang fails the test
     * rather than spinning. */
    int faulted = 0;
    for (int i = 0; i < 10 && !faulted; ++i) {
        memset(in, 0x11, sizeof(in));
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_ERR_TIMEOUT);
        CHECK_EQ_INT(in[0], last_good);   /* HOLD policy: held either side */
        faulted = (plc_adapter_state(a) == PLC_ADAPTER_FAULTED);
    }
    CHECK(faulted);

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

    test_adapter_exits_when_the_core_stops();
    TEST_REPORT("eip_ipc");
}
