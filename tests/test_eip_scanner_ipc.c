/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_eip_scanner_ipc.c
 * @brief End-to-end scanner test across the crash-containment boundary.
 *
 * Runs the real scanner binary in a real child process, driven through the
 * real proxy. Two things are under test that the adapter tests cannot cover:
 *
 *  - the **aggregate image layout** — four devices' slices packed in table
 *    order behind a health block, addressed by a POU as ordinary %I;
 *  - **per-device failsafe** — one device dropping must failsafe its slice
 *    only, under its own policy, while the other three keep running. That is
 *    the condition the whole health-block design exists for, and a single
 *    adapter state cannot express it.
 *
 * With the mirror backend (SOFTPLC_WITH_EIPSCANNER=OFF, what CI runs) no
 * network and no remote devices are involved, and every device reports online
 * so the payload and health assertions are exact. With the real stack linked
 * the devices in the table are not there, so those assertions are gated and
 * only the transport-level ones run.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "eip_scanner_shm_layout.h"
#include "softplc/adapter_registry.h"
#include "softplc/protocol_adapter.h"
#include "test_util.h"

#ifndef EIP_SCANNER_PATH
#  error "EIP_SCANNER_PATH must name the scanner binary"
#endif

/* Four devices, matching the deployment this was built for. Sizes differ per
 * device on purpose: equal sizes would hide an offset bug. */
static const char kTable[] =
    "127.0.0.1  151 150 100  8  8   10000 hold\n"
    "127.0.0.2  151 150 100  4  4   10000 clear\n"
    "127.0.0.3  151 150 100 16 16   10000 hold\n"
    "127.0.0.4  151 150 100  4  4   10000 hold\n";

#define DEVICES      4
#define HEALTH_BYTES DEVICES
#define O2T_TOTAL    (8 + 4 + 16 + 4)
#define T2O_TOTAL    (8 + 4 + 16 + 4)
#define IN_BYTES     (HEALTH_BYTES + T2O_TOTAL)

/* Offsets into the T->O data section, i.e. %I after the health block. */
static const size_t kT2oOffset[DEVICES] = { 0, 8, 12, 28 };
static const size_t kT2oLen[DEVICES]    = { 8, 4, 16, 4 };
static const size_t kO2tOffset[DEVICES] = { 0, 8, 12, 28 };

static void sleep_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static char g_table_path[128];

static void write_table(void) {
    snprintf(g_table_path, sizeof(g_table_path),
             "/tmp/softplc-scan-%d.conf", (int)getpid());
    FILE *f = fopen(g_table_path, "we");
    if (f) { fputs(kTable, f); fclose(f); }
}

static pid_t spawn_scanner(const char *instance, const char *down_mask) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (down_mask) setenv("SOFTPLC_SCANNER_MIRROR_DOWN", down_mask, 1);
        execl(EIP_SCANNER_PATH, "softplc-eip-scanner",
              instance, g_table_path, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int exchange_until_online(plc_protocol_adapter_t *a,
                                 const uint8_t *out, uint8_t *in, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (plc_adapter_exchange(a, out, O2T_TOTAL, in, IN_BYTES) == PLC_OK) return 1;
        sleep_ms(20);
    }
    return 0;
}

static plc_protocol_adapter_t *open_scanner(const plc_adapter_factory_t *f,
                                            const char *instance) {
    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name     = "scan-test";
    cfg.endpoint = instance;
    cfg.failsafe_policy = PLC_FAILSAFE_HOLD;
    cfg.exchange_timeout_us = 200000;
    cfg.failsafe_timeout_us = 300000;
    CHECK_EQ_INT(plc_adapter_open(a, &cfg), PLC_OK);
    return a;
}

/** All four devices healthy: the aggregate layout must round-trip per slice. */
static void test_aggregate_layout(void) {
    char instance[EIP_SCANNER_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "scan%d", (int)getpid());

    const plc_adapter_factory_t *f =
        plc_adapter_registry_find("ethernet-ip-scanner");
    plc_protocol_adapter_t *a = open_scanner(f, instance);

    plc_adapter_caps_t caps;
    plc_adapter_get_caps(a, &caps);
    CHECK_EQ_INT(caps.output_bytes, O2T_TOTAL);
    CHECK_EQ_INT(caps.input_bytes, IN_BYTES);   /* health block included */

    const pid_t child = spawn_scanner(instance, NULL);
    CHECK(child > 0);

    uint8_t out[O2T_TOTAL], in[IN_BYTES];
    /* A distinct byte per device, so a mis-packed slice is unmistakable. */
    for (int d = 0; d < DEVICES; ++d) {
        memset(out + kO2tOffset[d], (uint8_t)(0xA0 + d), kT2oLen[d]);
    }
    memset(in, 0, sizeof(in));

    CHECK(exchange_until_online(a, out, in, 200));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);

#if SOFTPLC_SCANNER_MIRROR_BACKEND
    for (int d = 0; d < DEVICES; ++d) {
        CHECK_EQ_INT(in[d], EIP_DEVICE_ONLINE);
        const uint8_t *slice = in + HEALTH_BYTES + kT2oOffset[d];
        for (size_t b = 0; b < kT2oLen[d]; ++b) {
            CHECK_EQ_INT(slice[b], (uint8_t)(0xA0 + d));
        }
    }
#else
    /* With the real stack and nothing at those addresses, no device connects.
     * The contract still holds: the health block is well formed, and every
     * slice is fully written rather than left undefined - a never-connected
     * device holds the zeros it started from. */
    for (int d = 0; d < DEVICES; ++d) {
        CHECK(in[d] == EIP_DEVICE_OFFLINE || in[d] == EIP_DEVICE_FAILSAFE);
        const uint8_t *slice = in + HEALTH_BYTES + kT2oOffset[d];
        for (size_t b = 0; b < kT2oLen[d]; ++b) CHECK_EQ_INT(slice[b], 0x00);
    }
#endif

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    plc_adapter_close(a);
    f->destroy(a);
}

/**
 * Device 1 (CLEAR) and device 2 (HOLD) forced down, devices 0 and 3 healthy.
 *
 * This is the case the design exists for: the adapter is ONLINE throughout,
 * because the scanner process is answering perfectly well. Only the health
 * bytes and the affected slices say otherwise.
 */
#if SOFTPLC_SCANNER_MIRROR_BACKEND
static void test_per_device_failsafe(void) {
    char instance[EIP_SCANNER_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "scand%d", (int)getpid());

    const plc_adapter_factory_t *f =
        plc_adapter_registry_find("ethernet-ip-scanner");
    plc_protocol_adapter_t *a = open_scanner(f, instance);

    /* bit 1 = device 1 (clear), bit 2 = device 2 (hold) */
    const pid_t child = spawn_scanner(instance, "6");
    CHECK(child > 0);

    uint8_t out[O2T_TOTAL], in[IN_BYTES];
    for (int d = 0; d < DEVICES; ++d) {
        memset(out + kO2tOffset[d], (uint8_t)(0xB0 + d), kT2oLen[d]);
    }
    memset(in, 0, sizeof(in));

    CHECK(exchange_until_online(a, out, in, 200));
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)), PLC_OK);

    /* The adapter itself is healthy - the peer answered on time. A single
     * adapter state could not have told us anything about the two dead
     * devices, which is the whole argument for the health block. */
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_ONLINE);

    CHECK_EQ_INT(in[0], EIP_DEVICE_ONLINE);
    CHECK_EQ_INT(in[1], EIP_DEVICE_FAILSAFE);
    CHECK_EQ_INT(in[2], EIP_DEVICE_FAILSAFE);
    CHECK_EQ_INT(in[3], EIP_DEVICE_ONLINE);

    /* Healthy devices carry their data, untouched by their neighbours' faults. */
    for (int d = 0; d < DEVICES; d += 3) {
        const uint8_t *slice = in + HEALTH_BYTES + kT2oOffset[d];
        for (size_t b = 0; b < kT2oLen[d]; ++b) {
            CHECK_EQ_INT(slice[b], (uint8_t)(0xB0 + d));
        }
    }

    /* Device 1 is CLEAR: zeroed. Device 2 is HOLD: it never received anything,
     * so its held image is the zero it started from - the distinguishing case
     * is covered by the parser and mirror-backend logic rather than here. */
    const uint8_t *dev1 = in + HEALTH_BYTES + kT2oOffset[1];
    for (size_t b = 0; b < kT2oLen[1]; ++b) CHECK_EQ_INT(dev1[b], 0x00);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    plc_adapter_close(a);
    f->destroy(a);
}
#endif /* SOFTPLC_SCANNER_MIRROR_BACKEND */

/** The scanner process dying is the coarse case: the adapter-level failsafe. */
static void test_process_loss(void) {
    char instance[EIP_SCANNER_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "scank%d", (int)getpid());

    const plc_adapter_factory_t *f =
        plc_adapter_registry_find("ethernet-ip-scanner");
    plc_protocol_adapter_t *a = open_scanner(f, instance);

    const pid_t child = spawn_scanner(instance, NULL);
    CHECK(child > 0);

    uint8_t out[O2T_TOTAL], in[IN_BYTES];
    memset(out, 0x5A, sizeof(out));
    memset(in, 0, sizeof(in));
    CHECK(exchange_until_online(a, out, in, 200));

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    const uint8_t held = in[HEALTH_BYTES];
    CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                 PLC_ERR_TIMEOUT);
    CHECK_EQ_INT(plc_adapter_state(a), PLC_ADAPTER_DEGRADED);

    int faulted = 0;
    for (int i = 0; i < 10 && !faulted; ++i) {
        CHECK_EQ_INT(plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)),
                     PLC_ERR_TIMEOUT);
        faulted = (plc_adapter_state(a) == PLC_ADAPTER_FAULTED);
    }
    CHECK(faulted);
    CHECK_EQ_INT(in[HEALTH_BYTES], held);   /* HOLD across the whole image */

    plc_adapter_stats_t s;
    plc_adapter_get_stats(a, &s);
    CHECK_EQ_INT(s.failsafe_activations, 1);

    plc_adapter_close(a);
    f->destroy(a);
}

int main(void) {
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();
    if (!plc_adapter_registry_find("ethernet-ip-scanner")) {
        printf("scanner adapter not built; skipping\n");
        return EXIT_SUCCESS;
    }

    write_table();
    setenv("SOFTPLC_SCANNER_DEVICES", g_table_path, 1);

    test_aggregate_layout();
#if SOFTPLC_SCANNER_MIRROR_BACKEND
    /* Forcing a device down is a mirror-backend hook; with the real stack you
     * would have to unplug an actual drive. */
    test_per_device_failsafe();
#endif
    test_process_loss();

    unlink(g_table_path);
    TEST_REPORT("eip_scanner_ipc");
}
