/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bench_exchange.c
 * @brief Phase 0 golden data: what does one cyclic exchange actually cost?
 *
 * `consecutive_timeout_threshold` and `exchange_timeout_us` shipped as
 * placeholders. This measures the thing they guard so they can be set from
 * data instead.
 *
 * What it measures: the **round trip of the IPC path** - core pushes a frame,
 * posts the doorbell, the adapter process wakes, swaps images and replies,
 * the core matches the sequence and copies the input image. That is exactly
 * what `exchange()`'s budget covers and exactly what a timeout means, so it
 * is the right quantity for both settings.
 *
 * What it does NOT measure: anything on the wire. The adapter runs its mirror
 * backend, so no CIP, no NIC, no scanner. Fieldbus latency lives inside the
 * adapter's own connection timeouts, not inside `exchange()` - the core's
 * budget is about whether the *peer process* answers, not whether a drive
 * did. Numbers from a real network do not change what this bounds.
 *
 * Usage: bench_exchange <adapter-binary> <scans> <cycle_us> [tag]
 */
#define _GNU_SOURCE
#include <errno.h>
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

#define IMAGE_BYTES 32
/* One microsecond per bucket up to 20 ms, which is four times the current
 * default budget - anything past that is already a timeout by definition. */
#define HIST_BUCKETS 20000

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_until_us(uint64_t deadline) {
    struct timespec ts = { .tv_sec  = (time_t)(deadline / 1000000u),
                           .tv_nsec = (long)(deadline % 1000000u) * 1000L };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

static uint64_t percentile(const uint64_t *hist, uint64_t total, double p) {
    const uint64_t want = (uint64_t)(total * p);
    uint64_t seen = 0;
    for (int i = 0; i < HIST_BUCKETS; ++i) {
        seen += hist[i];
        if (seen >= want) return (uint64_t)i;
    }
    return HIST_BUCKETS;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <adapter-binary> <scans> <cycle_us> [budget_us] [tag]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    const char *adapter_bin = argv[1];
    const long  scans       = strtol(argv[2], NULL, 10);
    const long  cycle_us    = strtol(argv[3], NULL, 10);
    const long  budget_us   = (argc > 4) ? strtol(argv[4], NULL, 10) : 20000;
    const char *tag         = (argc > 5) ? argv[5] : "run";

    plc_adapter_registry_reset();
    plc_adapter_register_builtins();
    const plc_adapter_factory_t *f = plc_adapter_registry_find("ethernet-ip");
    if (!f) { fprintf(stderr, "no ethernet-ip adapter\n"); return EXIT_FAILURE; }

    char instance[EIP_INSTANCE_MAX];
    snprintf(instance, sizeof(instance), "bench%d", (int)getpid());

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name        = "bench";
    cfg.endpoint    = instance;
    cfg.input_bytes = cfg.output_bytes = IMAGE_BYTES;
    /* Budget under evaluation. Wide by default so a run measures the whole
     * distribution; set it to a candidate value to see what that value would
     * actually reject, and how many rejections land back to back. */
    cfg.exchange_timeout_us = (uint32_t)budget_us;
    cfg.consecutive_timeout_threshold = 1000000;
    if (plc_adapter_open(a, &cfg) != PLC_OK) {
        fprintf(stderr, "open failed\n");
        return EXIT_FAILURE;
    }

    const pid_t child = fork();
    if (child == 0) {
        execl(adapter_bin, "softplc-eip-adapter", instance, "lo", (char *)NULL);
        _exit(127);
    }

    static uint64_t hist[HIST_BUCKETS];
    uint8_t out[IMAGE_BYTES], in[IMAGE_BYTES];
    memset(out, 0, sizeof(out));

    /* Warm up until the peer is serving; those exchanges are start-up cost,
     * not steady state, and including them would skew the tail. */
    for (int i = 0; i < 500; ++i) {
        if (plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in)) == PLC_OK) break;
        usleep(20000);
    }

    uint64_t timeouts = 0, max_rtt = 0, sum = 0, n = 0;
    /* The statistic the threshold actually rests on. An isolated miss is
     * absorbed by holding; only a *run* of them should reach failsafe, so what
     * matters is how long a run the environment produces on its own. */
    uint64_t run = 0, max_run = 0;
    uint64_t deadline = now_us();

    for (long s = 0; s < scans; ++s) {
        memset(out, (uint8_t)s, sizeof(out));
        const uint64_t t0 = now_us();
        const plc_status_t st = plc_adapter_exchange(a, out, sizeof(out), in, sizeof(in));
        const uint64_t rtt = now_us() - t0;

        if (st != PLC_OK) {
            timeouts++;
            if (++run > max_run) max_run = run;
        } else {
            run = 0;
            hist[rtt < HIST_BUCKETS ? rtt : HIST_BUCKETS - 1]++;
            if (rtt > max_rtt) max_rtt = rtt;
            sum += rtt;
            n++;
        }
        deadline += (uint64_t)cycle_us;
        const uint64_t t = now_us();
        if (deadline < t) deadline = t; else sleep_until_us(deadline);
    }

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    plc_adapter_close(a);
    f->destroy(a);

    if (n == 0) { fprintf(stderr, "no successful exchanges\n"); return EXIT_FAILURE; }

    printf("%-10s n=%-7llu mean=%-5llu p50=%-5llu p90=%-5llu p99=%-5llu "
           "p99.9=%-6llu p99.99=%-6llu max=%-6llu timeouts=%llu max_run=%llu\n",
           tag,
           (unsigned long long)n,
           (unsigned long long)(sum / n),
           (unsigned long long)percentile(hist, n, 0.50),
           (unsigned long long)percentile(hist, n, 0.90),
           (unsigned long long)percentile(hist, n, 0.99),
           (unsigned long long)percentile(hist, n, 0.999),
           (unsigned long long)percentile(hist, n, 0.9999),
           (unsigned long long)max_rtt,
           (unsigned long long)timeouts,
           (unsigned long long)max_run);
    return EXIT_SUCCESS;
}
