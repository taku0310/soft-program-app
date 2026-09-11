/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file e2e_cip_failsafe.c
 * @brief Does the failsafe hold the right *bytes* when a real CIP link drops?
 *
 * The suite verifies HOLD and CLEAR by killing the stack process, which tests
 * the IPC boundary: the proxy stops getting answers and applies the policy to
 * the image. What it cannot test is the other side of that boundary - a peer
 * that is alive and answering while the CIP connection underneath it has gone.
 * There the scanner keeps replying with an aggregate image, per-device
 * failsafe has already been applied inside it, and the question becomes
 * whether the bytes a POU reads are the last good ones or zeros.
 *
 * So this runs the Scanner role against a real Adapter, watches the device's
 * health byte, and from the moment it stops being 1 compares the device's data
 * slice against the last slice seen while it was online - on every scan the
 * link is down, not only the first. A link that flaps re-arms the comparison
 * against the newer online value, which is what the policy promises.
 *
 *   e2e_cip_failsafe <instance> <devices.conf> <scans> <hold|clear> [norecover]
 *
 * With `norecover` the caller promises the outage lasts to the end of the run,
 * so any return to ONLINE after the link drops is a device reporting healthy
 * with no data behind it - a ForwardOpen that succeeded over TCP while the
 * I/O path stayed broken. That is a failure, not a recovery.
 *
 * Exits non-zero unless the link came up, went down, and the image did what
 * the policy says. Nothing is asserted about *when* it went down; that is the
 * caller's business - it is the one causing the outage.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "softplc/adapter_registry.h"
#include "softplc/ipc/spsc_ring.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

#define MAX_IMG PLC_IPC_MAX_FRAME_BYTES

static void sleep_until_us(uint64_t deadline_us) {
    struct timespec ts = { .tv_sec  = (time_t)(deadline_us / 1000000u),
                           .tv_nsec = (long)(deadline_us % 1000000u) * 1000L };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <instance> <devices.conf> <scans> <hold|clear>\n",
                argv[0]);
        return 2;
    }
    const char *instance = argv[1];
    const long  scans    = strtol(argv[3], NULL, 10);
    const int   expect_clear = (strcmp(argv[4], "clear") == 0);
    const int   no_recover   = (argc > 5 && strcmp(argv[5], "norecover") == 0);

    setenv("SOFTPLC_SCANNER_DEVICES", argv[2], 1);
    plc_log_init("cipfs");
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    const plc_adapter_factory_t *f =
        plc_adapter_registry_find("ethernet-ip-scanner");
    if (!f) { fprintf(stderr, "no scanner adapter built\n"); return EXIT_FAILURE; }

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name     = "cipfs";
    cfg.endpoint = instance;
    if (plc_adapter_open(a, &cfg) != PLC_OK) {
        fprintf(stderr, "open failed\n");
        return EXIT_FAILURE;
    }

    plc_adapter_caps_t caps;
    plc_adapter_get_caps(a, &caps);

    uint8_t out[MAX_IMG], in[MAX_IMG];
    memset(out, 0, sizeof(out));
    memset(in, 0, sizeof(in));

    /* Device 0's data sits after the health bytes; one device, so one byte. */
    const uint32_t health_bytes = 1;
    const uint32_t data_off = health_bytes;
    const uint32_t data_len = (caps.input_bytes > data_off)
                            ? caps.input_bytes - data_off : 0;

    uint8_t last_online[MAX_IMG];
    memset(last_online, 0, sizeof(last_online));

    int  saw_online = 0, saw_down = 0;
    uint64_t online_scans = 0, down_scans = 0, violations = 0, false_healthy = 0;
    uint8_t down_health = 0xFF;

    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t deadline = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;

    for (long s = 0; s < scans; ++s) {
        /* A changing output so the peer's reply cannot be a stuck buffer. */
        for (uint32_t i = 0; i < caps.output_bytes; ++i) {
            out[i] = (uint8_t)(s + i);
        }
        plc_adapter_exchange(a, out, caps.output_bytes, in, caps.input_bytes);

        const uint8_t health = in[0];
        if (health == 1u) {
            if (saw_down) false_healthy++;
            saw_online = 1;
            online_scans++;
            memcpy(last_online, in + data_off, data_len);
        } else if (saw_online) {
            /* Every scan the link is down has to obey the policy, not just the
             * first: holding for one scan and then drifting is still a wrong
             * image in front of a POU. */
            int all_zero = 1, same_as_last = 1;
            for (uint32_t i = 0; i < data_len; ++i) {
                if (in[data_off + i] != 0) all_zero = 0;
                if (in[data_off + i] != last_online[i]) same_as_last = 0;
            }
            if (!(expect_clear ? all_zero : same_as_last)) violations++;
            if (!saw_down) {
                saw_down = 1;
                down_health = health;
                printf("cipfs: link down at scan %ld (health=%u) "
                       "all_zero=%d unchanged=%d\n",
                       s, health, all_zero, same_as_last);
            }
            down_scans++;
        }
        deadline += 10000;
        sleep_until_us(deadline);
    }

    printf("cipfs: policy=%s online_scans=%llu down_scans=%llu "
           "violations=%llu false_healthy=%llu data_len=%u\n",
           expect_clear ? "CLEAR" : "HOLD",
           (unsigned long long)online_scans, (unsigned long long)down_scans,
           (unsigned long long)violations, (unsigned long long)false_healthy,
           data_len);

    plc_adapter_close(a);
    f->destroy(a);

    if (!saw_online) { printf("cipfs: FAIL - never came online\n"); return EXIT_FAILURE; }
    if (!saw_down)   { printf("cipfs: FAIL - link never went down\n"); return EXIT_FAILURE; }
    if (no_recover && false_healthy) {
        printf("cipfs: FAIL - %llu scans reported ONLINE after the link went "
               "down and stayed down\n", (unsigned long long)false_healthy);
        return EXIT_FAILURE;
    }
    if (violations) {
        printf("cipfs: FAIL - %llu of %llu down scans did not follow %s "
               "(health was %u)\n",
               (unsigned long long)violations, (unsigned long long)down_scans,
               expect_clear ? "CLEAR" : "HOLD", down_health);
        return EXIT_FAILURE;
    }
    printf("cipfs: PASS - all %llu down scans followed %s across a real "
           "CIP connection loss\n",
           (unsigned long long)down_scans, expect_clear ? "CLEAR" : "HOLD");
    return EXIT_SUCCESS;
}
