/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_scan_overrun.c
 * @brief What the engine does when a scan does not fit in its period.
 *
 * The counter existed; the behaviour it counts did not. For a PLC the answer
 * matters more than the number, because there are two plausible ones and they
 * differ where it hurts:
 *
 *   - **Catch up.** Keep the absolute deadline and run the backlog
 *     back-to-back. The average period stays nominal, at the cost of a burst
 *     of scans crammed into no real time at all - so outputs change several
 *     times before the field sees any of them, and a POU that counts scans
 *     counts a burst.
 *   - **Lose the cycles.** Re-base the deadline on now and carry on. The
 *     period is missed, which is honest, and nothing is ever executed faster
 *     than real time.
 *
 * This runtime does the second (plc_runtime.c, `deadline = t`), and this test
 * holds it to that. The third assertion is the one that makes it safe: losing
 * cycles must not distort *time*, because `dt` is measured between scan
 * starts rather than assumed to be the period, so a TON still expires when it
 * should have in the real world however many cycles were dropped around it.
 */
#include "test_util.h"

#include <stdlib.h>
#include <time.h>

#include "softplc/plc_runtime.h"
#include "softplc/std_fb.h"

#define CYCLE_US   2000u    /* asked for */
#define WORK_US    6000u    /* actually taken: three times the period */
#define SCANS        100u

typedef struct {
    plc_ton_t timer;
    uint64_t  scans;
    PLC_TIME  dt_total;        /* sum of what the POU was told elapsed */
    uint64_t  q_at_scan;       /* scan on which the timer completed     */
    PLC_TIME  q_at_dt_total;   /* accumulated dt at that moment         */
} overrun_state_t;

static void busy_us(unsigned us) {
    struct timespec ts = { .tv_sec = us / 1000000u,
                           .tv_nsec = (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

static void slow_pou(plc_pou_ctx_t *ctx) {
    overrun_state_t *st = ctx->user;
    st->scans++;
    st->dt_total += ctx->dt;

    st->timer.IN = true;
    plc_ton_run(&st->timer, ctx->dt);
    if (st->timer.Q && st->q_at_scan == 0) {
        st->q_at_scan     = st->scans;
        st->q_at_dt_total = st->dt_total;
    }

    busy_us(WORK_US);
}

int main(void) {
    plc_runtime_config_t rc;
    plc_runtime_config_init(&rc);
    rc.cycle_us = CYCLE_US;

    plc_runtime_t *rt = plc_runtime_create(&rc);
    CHECK(rt != NULL);
    if (!rt) return EXIT_FAILURE;

    static overrun_state_t st;
    memset(&st, 0, sizeof(st));
    st.timer.PT = PLC_TIME_MS(200);

    CHECK_EQ_INT(plc_runtime_add_program(rt, "slow", slow_pou, &st), PLC_OK);

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    CHECK_EQ_INT(plc_runtime_run(rt, SCANS), PLC_OK);
    clock_gettime(CLOCK_MONOTONIC, &b);

    const uint64_t elapsed_us = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000
                                         + (b.tv_nsec - a.tv_nsec) / 1000);

    plc_runtime_stats_t s;
    plc_runtime_get_stats(rt, &s);

    CHECK_EQ_INT(s.scans, SCANS);
    CHECK_EQ_INT(st.scans, SCANS);

    /* 1. The overrun is noticed rather than silently absorbed. */
    CHECK(s.overruns >= SCANS - 5);
    CHECK(s.max_scan_us >= WORK_US);

    /* 2. No catching up. A hundred scans that each need 6 ms cannot be done
     *    in the 200 ms the nominal period suggests; anything near that would
     *    mean the engine had run them back to back. */
    const uint64_t honest_us = (uint64_t)SCANS * WORK_US;
    CHECK(elapsed_us >= honest_us * 8 / 10);
    if (elapsed_us < honest_us * 8 / 10) {
        fprintf(stderr, "      elapsed %lluus for %u scans of %uus\n",
                (unsigned long long)elapsed_us, SCANS, WORK_US);
    }

    /* 3. Lost cycles must not distort time. What the POU was told elapsed has
     *    to match the wall clock, not the period it asked for - otherwise a
     *    TON on a loaded PLC would expire late by however many cycles were
     *    dropped. */
    const uint64_t dt_total_us = (uint64_t)(st.dt_total / 1000);
    const uint64_t drift_us = (dt_total_us > elapsed_us)
                            ? dt_total_us - elapsed_us : elapsed_us - dt_total_us;
    CHECK(drift_us < elapsed_us / 10);
    if (drift_us >= elapsed_us / 10) {
        fprintf(stderr, "      dt summed to %lluus against %lluus of wall clock\n",
                (unsigned long long)dt_total_us, (unsigned long long)elapsed_us);
    }

    /* And the timer itself: 200 ms of PT reached at 200 ms of real time,
     *  around scan 33 rather than scan 100. */
    CHECK(st.q_at_scan > 0);
    if (st.q_at_scan) {
        const uint64_t q_us = (uint64_t)(st.q_at_dt_total / 1000);
        CHECK(q_us >= 195000 && q_us <= 215000);
        if (!(q_us >= 195000 && q_us <= 215000)) {
            fprintf(stderr, "      TON(200ms) completed at %lluus, scan %llu\n",
                    (unsigned long long)q_us, (unsigned long long)st.q_at_scan);
        }
    }

    plc_runtime_destroy(rt);
    TEST_REPORT("scan_overrun");
}
