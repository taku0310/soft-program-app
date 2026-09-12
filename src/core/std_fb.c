/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/std_fb.h"

/* --- edge detection ------------------------------------------------------ */

void plc_r_trig_run(plc_r_trig_t *fb) {
    if (!fb) return;
    fb->Q = (PLC_BOOL)(fb->CLK && !fb->m_prev);
    fb->m_prev = fb->CLK;
}

void plc_f_trig_run(plc_f_trig_t *fb) {
    if (!fb) return;
    fb->Q = (PLC_BOOL)(!fb->CLK && fb->m_prev);
    fb->m_prev = fb->CLK;
}

/* --- bistables ----------------------------------------------------------- */

void plc_rs_run(plc_rs_t *fb) {           /* reset dominant */
    if (!fb) return;
    fb->Q1 = (PLC_BOOL)(!fb->R1 && (fb->S || fb->Q1));
}

void plc_sr_run(plc_sr_t *fb) {           /* set dominant */
    if (!fb) return;
    fb->Q1 = (PLC_BOOL)(fb->S1 || (!fb->R && fb->Q1));
}

/* --- timers --------------------------------------------------------------
 *
 * ET saturates at PT rather than free-running.  IEC 61131-3 requires ET to
 * stop at PT, and saturating also keeps the accumulator away from overflow on
 * a machine that runs for months.
 */

static PLC_TIME accumulate(PLC_TIME et, PLC_TIME dt, PLC_TIME pt) {
    if (dt < 0) dt = 0;
    if (et >= pt) return pt;
    et += dt;
    return (et > pt) ? pt : et;
}

void plc_ton_run(plc_ton_t *fb, PLC_TIME dt) {
    if (!fb) return;
    if (!fb->IN) { fb->ET = 0; fb->Q = false; return; }
    fb->ET = accumulate(fb->ET, dt, fb->PT);
    fb->Q  = (PLC_BOOL)(fb->ET >= fb->PT);
}

void plc_tof_run(plc_tof_t *fb, PLC_TIME dt) {
    if (!fb) return;
    if (fb->IN) {
        fb->ET = 0;
        fb->Q  = true;
        fb->m_running = false;
    } else {
        if (fb->m_prev) {             /* falling edge starts the delay */
            fb->ET = 0;
            fb->m_running = true;
        }
        /* Only accumulate while the delay is actually running.  Deriving that
         * from ET < PT instead would make a zero-initialised block - one whose
         * IN has never been true - report Q for the first PT after start-up,
         * which is not the IEC initial state and would energise a permissive
         * on every restart. */
        if (fb->m_running) {
            fb->ET = accumulate(fb->ET, dt, fb->PT);
            if (fb->ET >= fb->PT) {
                fb->Q = false;
                fb->m_running = false;
            }
        }
    }
    fb->m_prev = fb->IN;
}

void plc_tp_run(plc_tp_t *fb, PLC_TIME dt) {
    if (!fb) return;
    if (!fb->Q && fb->IN && !fb->m_prev) {   /* rising edge, not retriggerable */
        fb->Q  = true;
        fb->ET = 0;
    } else if (fb->Q) {
        fb->ET = accumulate(fb->ET, dt, fb->PT);
        if (fb->ET >= fb->PT) fb->Q = false;
    } else if (!fb->IN) {
        fb->ET = 0;
    }
    fb->m_prev = fb->IN;
}

/* --- counters ------------------------------------------------------------ */

void plc_ctu_run(plc_ctu_t *fb) {
    if (!fb) return;
    if (fb->R) {
        fb->CV = 0;
    } else if (fb->CU && !fb->m_prev && fb->CV < INT16_MAX) {
        fb->CV++;
    }
    fb->Q = (PLC_BOOL)(fb->CV >= fb->PV);
    fb->m_prev = fb->CU;
}

void plc_ctd_run(plc_ctd_t *fb) {
    if (!fb) return;
    if (fb->LD) {
        fb->CV = fb->PV;
    } else if (fb->CD && !fb->m_prev && fb->CV > INT16_MIN) {
        fb->CV--;
    }
    fb->Q = (PLC_BOOL)(fb->CV <= 0);
    fb->m_prev = fb->CD;
}

void plc_ctud_run(plc_ctud_t *fb) {
    if (!fb) return;
    const PLC_BOOL up   = (PLC_BOOL)(fb->CU && !fb->m_prev_cu);
    const PLC_BOOL down = (PLC_BOOL)(fb->CD && !fb->m_prev_cd);

    if (fb->R) {
        fb->CV = 0;
    } else if (fb->LD) {
        fb->CV = fb->PV;
    } else {
        /* Simultaneous edges cancel, as required by the standard. */
        if (up && !down && fb->CV < INT16_MAX) fb->CV++;
        if (down && !up && fb->CV > INT16_MIN) fb->CV--;
    }
    fb->QU = (PLC_BOOL)(fb->CV >= fb->PV);
    fb->QD = (PLC_BOOL)(fb->CV <= 0);
    fb->m_prev_cu = fb->CU;
    fb->m_prev_cd = fb->CD;
}
