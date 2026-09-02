/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file e2e_two_plc.c
 * @brief One soft PLC core, driving one adapter, with its images visible.
 *
 * Two of these are run against each other - one in the Adapter role, one in
 * the Scanner role - to prove that real EtherNet/IP traffic flows between two
 * soft PLCs. `softplc` itself cannot show this: it runs a fixed demo program
 * and never prints the process image, so a passing run there proves only that
 * it did not crash.
 *
 * Each side writes a frame that changes every scan and reports what arrives on
 * its inputs. The frame is a 32-bit sequence number followed by bytes derived
 * from their own offset and the sender's tag, which makes three distinct
 * faults visible that a constant pattern cannot show:
 *
 *   - wrong peer, loopback or echo   -> the tag is not the peer's
 *   - a stale or repeated frame      -> the sequence number stops advancing
 *   - a byte-offset error in the map -> the per-index bytes do not match
 *
 * A constant fill would pass all three while broken, which is why the first
 * version of this harness was not enough.
 *
 * They must be on separate hosts, or separate network namespaces: CIP class 1
 * uses a fixed UDP port (2222) at both ends, so two EtherNet/IP endpoints on
 * one host fight over it and each receives its own transmissions.
 *
 * Usage: e2e_two_plc <adapter|scanner> <instance> <pattern> <scans>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "softplc/adapter_registry.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

#define MAX_IMG 512

static void sleep_until_us(uint64_t deadline) {
    struct timespec ts = { .tv_sec  = (time_t)(deadline / 1000000u),
                           .tv_nsec = (long)(deadline % 1000000u) * 1000L };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <adapter|scanner> <instance> <pattern> <scans>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    const char *role     = argv[1];
    const char *instance = argv[2];
    const uint8_t pattern = (uint8_t)strtoul(argv[3], NULL, 0);
    const long scans      = strtol(argv[4], NULL, 10);

    plc_log_init(role);
    plc_adapter_registry_reset();
    plc_adapter_register_builtins();

    const char *proto = (strcmp(role, "scanner") == 0) ? "ethernet-ip-scanner"
                                                       : "ethernet-ip";
    const plc_adapter_factory_t *f = plc_adapter_registry_find(proto);
    if (!f) { fprintf(stderr, "no %s adapter built\n", proto); return EXIT_FAILURE; }

    plc_protocol_adapter_t *a = f->create();
    plc_adapter_config_t cfg;
    plc_adapter_config_init(&cfg);
    cfg.name     = role;
    cfg.endpoint = instance;
    cfg.failsafe_policy = PLC_FAILSAFE_HOLD;
    if (plc_adapter_open(a, &cfg) != PLC_OK) {
        fprintf(stderr, "open failed\n");
        return EXIT_FAILURE;
    }

    plc_adapter_caps_t caps;
    plc_adapter_get_caps(a, &caps);

    uint8_t out[MAX_IMG], in[MAX_IMG];
    memset(out, 0, sizeof(out));
    memset(in, 0, sizeof(in));

    /* The scanner's input image leads with one health byte per device; the
     * adapter's does not. Skip past it so both roles report the same thing:
     * the first byte of actual field data. */
    const size_t data_off = (strcmp(role, "scanner") == 0) ? 1 : 0;

    /* Wait for the peer stack process to attach before counting anything.
     * Without this the figures are dominated by the start-up window - every
     * scan before the peer exists times out, which says nothing about steady
     * state and is easy to misread as a performance problem. */
    uint64_t warmup = 0;
    for (; warmup < 2000; ++warmup) {
        if (plc_adapter_exchange(a, out, caps.output_bytes,
                                 in, caps.input_bytes) == PLC_OK) break;
        struct timespec w = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&w, NULL);
    }
    if (warmup >= 2000) {
        fprintf(stderr, "%s: peer never answered\n", role);
        return EXIT_FAILURE;
    }

    uint64_t ok = 0, timeouts = 0, matched = 0;
    uint8_t seen = 0;
    uint32_t tx_seq = 0;
    uint32_t last_peer_seq = 0;
    int have_peer_seq = 0;
    uint64_t fresh_frames = 0;   /* peer's sequence advanced */
    uint64_t stale_frames = 0;   /* same frame seen again */
    uint64_t corrupt_bytes = 0;  /* per-index pattern mismatch */
    /* Sampled during the run, not at the end: whichever side finishes first
     * takes its stack down with it, so the *final* health byte reflects the
     * teardown rather than the run. */
    int ever_online = 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t deadline = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;

    for (long s = 0; s < scans; ++s) {
        /* Frame: [0..3] sequence, [4..] (index ^ our tag). Both change every
         * scan, so a repeat or a shifted copy is visible. */
        tx_seq++;
        out[0] = (uint8_t)(tx_seq      ); out[1] = (uint8_t)(tx_seq >>  8);
        out[2] = (uint8_t)(tx_seq >> 16); out[3] = (uint8_t)(tx_seq >> 24);
        for (uint32_t i = 4; i < caps.output_bytes; ++i) {
            out[i] = (uint8_t)(i ^ pattern);
        }

        const plc_status_t st = plc_adapter_exchange(a, out, caps.output_bytes,
                                                     in, caps.input_bytes);
        if (st == PLC_OK) ok++; else timeouts++;

        const uint8_t *rx = in + data_off;
        const uint32_t rx_len = (caps.input_bytes > data_off)
                              ? caps.input_bytes - (uint32_t)data_off : 0;

        if (rx_len >= 8) {
            const uint32_t peer_seq = (uint32_t)rx[0] | ((uint32_t)rx[1] << 8)
                                    | ((uint32_t)rx[2] << 16) | ((uint32_t)rx[3] << 24);
            /* The peer's tag is whatever it XORed into byte 4. Recovering it
             * this way means we do not have to be told the peer's pattern. */
            const uint8_t peer_tag = (uint8_t)(rx[4] ^ 4u);
            seen = peer_tag;

            if (peer_seq != 0) {
                if (have_peer_seq) {
                    if (peer_seq != last_peer_seq) fresh_frames++; else stale_frames++;
                }
                last_peer_seq = peer_seq;
                have_peer_seq = 1;

                if (peer_tag != pattern) {
                    matched++;
                    /* Every remaining byte must follow from the same tag. A
                     * one-byte shift anywhere in the map breaks this. */
                    for (uint32_t i = 5; i < rx_len; ++i) {
                        if (rx[i] != (uint8_t)(i ^ peer_tag)) { corrupt_bytes++; break; }
                    }
                }
            }
        }
        if (data_off && in[0] == 1u) ever_online = 1;
        deadline += 10000;
        sleep_until_us(deadline);
    }

    plc_adapter_stats_t stats;
    plc_adapter_get_stats(a, &stats);

    printf("%-8s tag=0x%02X  warmup=%llu  exchanges=%llu timeouts=%llu (%.1f%%)\n"
           "%-8s peer_tag=0x%02X  peer_frames=%llu  fresh=%llu stale=%llu  "
           "corrupt=%llu  max_rtt=%lluus  state=%s\n",
           role, pattern,
           (unsigned long long)warmup,
           (unsigned long long)ok, (unsigned long long)timeouts,
           (ok + timeouts) ? 100.0 * (double)timeouts / (double)(ok + timeouts) : 0.0,
           role, seen,
           (unsigned long long)matched,
           (unsigned long long)fresh_frames, (unsigned long long)stale_frames,
           (unsigned long long)corrupt_bytes,
           (unsigned long long)stats.max_rtt_us,
           plc_adapter_state_name(plc_adapter_state(a)));
    if (data_off) {
        printf("%-8s device health: reached online during the run = %s "
               "(final byte %u, sampled after the peer had gone)\n",
               role, ever_online ? "yes" : "NO", in[0]);
    }

    plc_adapter_close(a);
    f->destroy(a);

    /* Pass needs all three: the peer's data arrived, it kept changing, and
     * every byte landed where the map says it should. */
    const int pass = (matched > 0) && (fresh_frames > 0) && (corrupt_bytes == 0);
    if (!pass) {
        fprintf(stderr, "%s: FAIL (peer_frames=%llu fresh=%llu corrupt=%llu)\n",
                role, (unsigned long long)matched,
                (unsigned long long)fresh_frames,
                (unsigned long long)corrupt_bytes);
    }
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
