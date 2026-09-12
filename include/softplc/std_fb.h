/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file std_fb.h
 * @brief IEC 61131-3 standard function blocks (Annex F subset).
 *
 * Each block is a struct of IEC inputs/outputs plus private state, and a
 * <name>_run() that is called once per scan with the scan's elapsed time.
 * Timing is driven by the scan's @c dt rather than by reading a clock inside
 * the block, so behaviour is identical whether the runtime is scanning in real
 * time or being stepped by a test.
 */
#ifndef SOFTPLC_STD_FB_H
#define SOFTPLC_STD_FB_H

#include "softplc/plc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- edge detection ------------------------------------------------------ */

typedef struct plc_r_trig { PLC_BOOL CLK, Q; PLC_BOOL m_prev; } plc_r_trig_t;
typedef struct plc_f_trig { PLC_BOOL CLK, Q; PLC_BOOL m_prev; } plc_f_trig_t;

void plc_r_trig_run(plc_r_trig_t *fb);
void plc_f_trig_run(plc_f_trig_t *fb);

/* --- bistables ----------------------------------------------------------- */

/** Reset-dominant. */
typedef struct plc_rs { PLC_BOOL S, R1, Q1; } plc_rs_t;
/** Set-dominant. */
typedef struct plc_sr { PLC_BOOL S1, R, Q1; } plc_sr_t;

void plc_rs_run(plc_rs_t *fb);
void plc_sr_run(plc_sr_t *fb);

/* --- timers -------------------------------------------------------------- */

/** On-delay: Q goes true PT after IN went true. */
typedef struct plc_ton { PLC_BOOL IN, Q; PLC_TIME PT, ET; } plc_ton_t;
/** Off-delay: Q stays true PT after IN went false. */
typedef struct plc_tof {
    PLC_BOOL IN, Q; PLC_TIME PT, ET;
    PLC_BOOL m_prev;
    /** The delay is only running between a falling edge of IN and ET reaching
     *  PT.  Kept explicitly because ET alone cannot tell "never started" from
     *  "started and not finished": a zero-initialised block has ET == 0 and
     *  must report Q = FALSE, not "still timing out". */
    PLC_BOOL m_running;
} plc_tof_t;
/** Pulse: a rising IN produces a PT-long pulse, not retriggerable. */
typedef struct plc_tp  { PLC_BOOL IN, Q; PLC_TIME PT, ET; PLC_BOOL m_prev; } plc_tp_t;

void plc_ton_run(plc_ton_t *fb, PLC_TIME dt);
void plc_tof_run(plc_tof_t *fb, PLC_TIME dt);
void plc_tp_run (plc_tp_t  *fb, PLC_TIME dt);

/* --- counters ------------------------------------------------------------ */

typedef struct plc_ctu {
    PLC_BOOL CU, R, Q; PLC_INT PV, CV; PLC_BOOL m_prev;
} plc_ctu_t;

typedef struct plc_ctd {
    PLC_BOOL CD, LD, Q; PLC_INT PV, CV; PLC_BOOL m_prev;
} plc_ctd_t;

typedef struct plc_ctud {
    PLC_BOOL CU, CD, R, LD, QU, QD;
    PLC_INT  PV, CV;
    PLC_BOOL m_prev_cu, m_prev_cd;
} plc_ctud_t;

void plc_ctu_run (plc_ctu_t  *fb);
void plc_ctd_run (plc_ctd_t  *fb);
void plc_ctud_run(plc_ctud_t *fb);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_STD_FB_H */
