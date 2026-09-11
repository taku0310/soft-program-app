/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_eip_data_invalid.c
 * @brief A peer that answers on time and still has nothing valid to say.
 *
 * Two states reach that, and before this both looked to the PLC exactly like
 * good data arriving on time:
 *
 *   - **No I/O connection.** The consumed assembly holds whatever was last
 *     written, or zeros if no controller ever connected. Answering with that
 *     made "nobody is driving us" indistinguishable from "the controller is
 *     sending zeros" - and because the answer was prompt, the staleness clock
 *     was reset every scan, so the failsafe could never fire. A PLC that
 *     starts up with no controller attached and reads a valid-looking all-zero
 *     input image is a PLC that will act on it.
 *
 *   - **The originator is IDLE.** CIP's run/idle header is the controller
 *     stating that the data in this frame is not valid. OpENer reports the bit
 *     and then applies the payload to the assembly regardless
 *     (cipioconnection.c), so without this the outputs keep being driven from
 *     data the controller has disowned.
 *
 * Both must reach the configured failsafe, and neither may poison the held
 * image: a later real connection loss has to HOLD the last image that meant
 * something, not the idle one.
 *
 * The two states are forced through the mirror backend, so this runs in the
 * default build with no submodule and no NIC.
 */
#include "test_util.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "softplc/adapter_registry.h"
#include "softplc/protocol_adapter.h"

#ifndef EIP_ADAPTER_PATH
#  error "EIP_ADAPTER_PATH must name the adapter binary"
#endif
#ifndef SOFTPLC_EIP_MIRROR_BACKEND
#  define SOFTPLC_EIP_MIRROR_BACKEND 0
#endif

#define IMAGE_BYTES 16

static void sleep_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/** @p env is applied in the child only, so each case gets its own adapter. */
static pid_t spawn_adapter(const char *instance, const char *key, const char *val) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (key) setenv(key, val, 1);
        execl(EIP_ADAPTER_PATH, "softplc-eip-adapter", instance, "lo", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop_adapter(pid_t pid) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static plc_protocol_adapter_t *open_adapter(const plc_adapter_factory_t *f,
                                            const char *instance,
                                            plc_failsafe_policy_t policy) {
    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name            = "eip";
    cfg.endpoint        = instance;
    cfg.input_bytes     = IMAGE_BYTES;
    cfg.output_bytes    = IMAGE_BYTES;
    cfg.failsafe_policy = policy;
    /* Short, so a case that is meant to reach failsafe does so inside the
     * test rather than inside the timeout. */
    cfg.failsafe_timeout_us = 50000;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);
    return a;
}

/** Run @p n exchanges; report how many returned PLC_OK. */
static int pump(plc_protocol_adapter_t *a, const uint8_t *out, uint8_t *in, int n) {
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (plc_adapter_exchange(a, out, IMAGE_BYTES, in, IMAGE_BYTES) == PLC_OK) ok++;
        sleep_ms(5);
    }
    return ok;
}

/**
 * Nobody is connected: every exchange must be refused, and the image must not
 * come back as a plausible-looking block of zeros the program would act on.
 */
static void test_no_connection_is_not_valid_data(void) {
    char inst[64];
    snprintf(inst, sizeof(inst), "dinv-noconn-%d", (int)getpid());

    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    CHECK(f != NULL);
    if (!f) return;

    plc_protocol_adapter_t *a = open_adapter(f, inst, PLC_FAILSAFE_HOLD);
    const pid_t pid = spawn_adapter(inst, "SOFTPLC_EIP_MIRROR_NO_CONNECTION", "1");
    sleep_ms(300);

    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0xC5, sizeof(out));
    memset(in,  0xEE, sizeof(in));

    const int ok = pump(a, out, in, 30);
    CHECK_EQ_INT(ok, 0);          /* not one exchange may report usable data */

    plc_adapter_stats_t st;
    plc_adapter_get_stats(a, &st);
    CHECK(st.data_invalid > 0);
    /* Counted apart from timeouts: the peer answered every time. */
    CHECK_EQ_INT(st.timeouts, 0);
    /* HOLD with nothing ever held is all zeros - defined, and not mistakable
     * for live data because the exchange refused it. */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    stop_adapter(pid);
    plc_adapter_close(a);
    f->destroy(a);
}

#if SOFTPLC_EIP_MIRROR_BACKEND
/**
 * A connected controller that goes to IDLE. Good data flows first, so the
 * held image is real, and CLEAR is used so the transition is visible in the
 * image rather than only in a counter.
 */
static void test_idle_peer_applies_failsafe(void) {
    char inst[64];
    snprintf(inst, sizeof(inst), "dinv-idle-%d", (int)getpid());

    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    CHECK(f != NULL);
    if (!f) return;

    plc_protocol_adapter_t *a = open_adapter(f, inst, PLC_FAILSAFE_CLEAR);

    /* Phase 1: a peer in RUN. The mirror echoes the outputs, so a known
     * pattern proves real data reached the image. */
    pid_t pid = spawn_adapter(inst, NULL, NULL);
    sleep_ms(300);

    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0xA7, sizeof(out));
    memset(in,  0, sizeof(in));
    const int ok = pump(a, out, in, 30);
    CHECK(ok > 0);
    CHECK_EQ_INT(in[0], 0xA7);
    stop_adapter(pid);

    /* Phase 2: same connection parameters, peer now asserting IDLE. */
    pid = spawn_adapter(inst, "SOFTPLC_EIP_MIRROR_IDLE", "1");
    sleep_ms(300);

    plc_adapter_stats_t before;
    plc_adapter_get_stats(a, &before);

    memset(in, 0xEE, sizeof(in));
    const int ok2 = pump(a, out, in, 30);
    CHECK_EQ_INT(ok2, 0);

    plc_adapter_stats_t after;
    plc_adapter_get_stats(a, &after);
    CHECK(after.data_invalid > before.data_invalid);

    /* CLEAR, applied at once: an explicit "my data is invalid" is not the
     * same as a missing frame, and does not wait out the staleness window. */
    for (size_t i = 0; i < IMAGE_BYTES; ++i) CHECK_EQ_INT(in[i], 0);

    stop_adapter(pid);
    plc_adapter_close(a);
    f->destroy(a);
}
#endif /* SOFTPLC_EIP_MIRROR_BACKEND */

int main(void) {
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    /* Real in both builds: with OpENer linked and no scanner attached,
     * io_connections() is genuinely 0, which is the case that matters. */
    test_no_connection_is_not_valid_data();

#if SOFTPLC_EIP_MIRROR_BACKEND
    /* Forcing a peer to assert IDLE needs a peer we control. Against real
     * OpENer that takes a scanner that can be put into PROGRAM mode, which is
     * the hardware test this project has not been able to run. */
    test_idle_peer_applies_failsafe();
#else
    printf("  (idle-peer case skipped: needs the mirror backend)\n");
#endif
    TEST_REPORT("eip_data_invalid");
}
