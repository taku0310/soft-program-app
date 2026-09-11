/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file e2e_link_survive.c
 * @brief Does the link come back by itself after a real cable pull?
 *
 * B1b dropped UDP 2222 with iptables, which leaves the TCP session and its
 * socket intact underneath: only the class 1 I/O stops. A cable pull is not
 * that. The TCP session dies too, so recovery has to re-register the session
 * and issue a fresh ForwardOpen - a path nothing in this project had ever
 * executed, and A4 found a defect sitting in the half of it that iptables
 * does reach.
 *
 * So this runs the Scanner role and reports, in scans and in microseconds:
 * how long until the outage was visible in the image, whether the policy was
 * obeyed while it lasted, how long until the device was online again, and -
 * the part that separates recovery from a stuck buffer - whether the image
 * kept *changing* afterwards.
 *
 *   e2e_link_survive <instance> <devices.conf> <scans> <hold|clear>
 *
 * The caller owns the outage: down the link, wait, bring it back. This only
 * watches. It fails if the device never came online, never went down, broke
 * the policy while down, never came back, or came back stuck.
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

#define MAX_IMG   PLC_IPC_MAX_FRAME_BYTES
#define SCAN_US   10000u
/* Enough post-recovery scans to tell a live image from a frozen one. */
#define FRESH_MIN 20

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Printed alongside each transition so the caller - the only one who knows
 * when it pulled the cable - can turn them into detection and recovery
 * latencies without this process having to be told the schedule. */
static uint64_t wall_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

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
    const char *instance     = argv[1];
    const long  scans        = strtol(argv[3], NULL, 10);
    const int   expect_clear = (strcmp(argv[4], "clear") == 0);

    setenv("SOFTPLC_SCANNER_DEVICES", argv[2], 1);
    plc_log_init("linksurv");
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    const plc_adapter_factory_t *f =
        plc_adapter_registry_find("ethernet-ip-scanner");
    if (!f) { fprintf(stderr, "no scanner adapter built\n"); return EXIT_FAILURE; }

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name     = "linksurv";
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

    const uint32_t data_off = 1;                       /* one device, one health byte */
    const uint32_t data_len = (caps.input_bytes > data_off)
                            ? caps.input_bytes - data_off : 0;

    uint8_t last_online[MAX_IMG], prev[MAX_IMG];
    memset(last_online, 0, sizeof(last_online));
    memset(prev, 0, sizeof(prev));

    int saw_online = 0, saw_down = 0, saw_recovery = 0;
    long down_scan = -1, up_scan = -1;
    uint64_t down_us = 0, up_us = 0;
    uint64_t down_wall = 0, up_wall = 0;
    uint64_t online_scans = 0, down_scans = 0, violations = 0;
    uint64_t post_scans = 0, post_changes = 0;
    uint8_t  down_health = 0xFF;

    uint64_t deadline = now_us();

    for (long s = 0; s < scans; ++s) {
        for (uint32_t i = 0; i < caps.output_bytes; ++i) out[i] = (uint8_t)(s + i);
        plc_adapter_exchange(a, out, caps.output_bytes, in, caps.input_bytes);

        const uint8_t health = in[0];
        if (health == 1u) {
            if (saw_down && !saw_recovery) {
                saw_recovery = 1;
                up_scan = s;
                up_us   = now_us();
                up_wall = wall_us();
            }
            if (saw_recovery) {
                /* Back online is not the claim; back online and *moving* is.
                 * A frozen buffer would satisfy the health byte alone. */
                post_scans++;
                if (memcmp(in + data_off, prev, data_len) != 0) post_changes++;
            }
            saw_online = 1;
            online_scans++;
            memcpy(last_online, in + data_off, data_len);
        } else if (saw_online && !saw_recovery) {
            int all_zero = 1, same_as_last = 1;
            for (uint32_t i = 0; i < data_len; ++i) {
                if (in[data_off + i] != 0) all_zero = 0;
                if (in[data_off + i] != last_online[i]) same_as_last = 0;
            }
            if (!(expect_clear ? all_zero : same_as_last)) violations++;
            if (!saw_down) {
                saw_down    = 1;
                down_scan   = s;
                down_us     = now_us();
                down_wall   = wall_us();
                down_health = health;
            }
            down_scans++;
        }
        memcpy(prev, in + data_off, data_len);
        deadline += SCAN_US;
        sleep_until_us(deadline);
    }

    printf("linksurv: policy=%s online=%llu down=%llu violations=%llu\n",
           expect_clear ? "CLEAR" : "HOLD",
           (unsigned long long)online_scans, (unsigned long long)down_scans,
           (unsigned long long)violations);
    if (saw_down) {
        printf("linksurv: outage visible at scan %ld (health=%u) at_wall_us=%llu\n",
               down_scan, down_health, (unsigned long long)down_wall);
    }
    if (saw_recovery) {
        printf("linksurv: back online at scan %ld, %llu us after the image "
               "went down, at_wall_us=%llu; %llu of %llu later scans carried "
               "new data\n",
               up_scan, (unsigned long long)(up_us - down_us),
               (unsigned long long)up_wall,
               (unsigned long long)post_changes, (unsigned long long)post_scans);
    }

    plc_adapter_close(a);
    f->destroy(a);

    if (!saw_online)   { printf("linksurv: FAIL - never came online\n");        return EXIT_FAILURE; }
    if (!saw_down)     { printf("linksurv: FAIL - the outage never showed\n");  return EXIT_FAILURE; }
    if (violations)    { printf("linksurv: FAIL - %llu down scans broke the policy\n",
                                (unsigned long long)violations);                return EXIT_FAILURE; }
    if (!saw_recovery) { printf("linksurv: FAIL - never recovered\n");          return EXIT_FAILURE; }
    if (post_scans < FRESH_MIN) {
        printf("linksurv: FAIL - only %llu scans after recovery, too few to "
               "tell a live image from a frozen one\n",
               (unsigned long long)post_scans);
        return EXIT_FAILURE;
    }
    if (post_changes == 0) {
        printf("linksurv: FAIL - online again but the image never changed "
               "over %llu scans\n", (unsigned long long)post_scans);
        return EXIT_FAILURE;
    }
    printf("linksurv: PASS - outage seen, policy held, link re-established, "
           "image live again\n");
    return EXIT_SUCCESS;
}
