/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_eip_failsafe_clear.c
 * @brief The CLEAR half of the failsafe decision, across a real process death.
 *
 * Same shape as test_eip_ipc.c but configured for PLC_FAILSAFE_CLEAR, because
 * the two policies are the one place where the same event has to produce
 * deliberately different process images.  Testing only HOLD would leave the
 * selectable-policy requirement half covered.
 *
 * Telling HOLD from CLEAR needs a non-zero image to hold, which only the
 * mirror backend can supply; with the real stack linked and no scanner
 * connected the inputs are zero and the two policies are indistinguishable by
 * value.  The state machine is checked either way, the values only where they
 * carry information.
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

#define IMAGE_BYTES 8

static void sleep_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void) {
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    CHECK(f != NULL);
    if (!f) TEST_REPORT("eip_failsafe_clear");

    char instance[EIP_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "clr%d", (int)getpid());

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name        = "eip-clear";
    cfg.endpoint    = instance;
    cfg.input_bytes = cfg.output_bytes = IMAGE_BYTES;
    cfg.failsafe_policy = PLC_FAILSAFE_CLEAR;
    cfg.exchange_timeout_us = 200000;
    cfg.failsafe_timeout_us = 300000;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);

    const pid_t child = fork();
    if (child == 0) {
        execl(EIP_ADAPTER_PATH, "softplc-eip-adapter", instance, "lo", (char *)NULL);
        _exit(127);
    }
    CHECK(child > 0);

    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0x77, sizeof(out));
    memset(in, 0, sizeof(in));

    int online = 0;
    for (int i = 0; i < 200 && !online; ++i) {
        online = (plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)) == PLC_OK);
        if (!online) sleep_ms(20);
    }
    CHECK(online);
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);
#if SOFTPLC_EIP_MIRROR_BACKEND
    CHECK_EQ_INT(in[0], 0x77);
#endif
    const uint8_t last_good = in[0];

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    /* Inside the window it holds, even under CLEAR: that is the rule that
     * stops one dropped frame injecting a falling edge on every input. */
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(in[0], last_good);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    /* Past it, CLEAR means the whole image goes to zero. */
    int faulted = 0;
    for (int i = 0; i < 10 && !faulted; ++i) {
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_ERR_TIMEOUT);
        faulted = (plc_adapter_state(a) == PLC_ADAPTER_FAULTED);
    }
    CHECK(faulted);
    for (unsigned i = 0; i < sizeof(in); ++i) CHECK_EQ_INT(in[i], 0x00);

    plc_adapter_close(a);
    f->destroy(a);
    TEST_REPORT("eip_failsafe_clear");
}
