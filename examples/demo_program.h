/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SOFTPLC_DEMO_PROGRAM_H
#define SOFTPLC_DEMO_PROGRAM_H

#include "softplc/plc_runtime.h"
#include "softplc/std_fb.h"

/** State for the demo POU.  Zero-initialise before the first scan. */
typedef struct demo_state {
    plc_ton_t   start_delay;
    plc_tp_t    lamp_pulse;
    plc_r_trig_t part_edge;
    plc_ctu_t   part_counter;
    plc_rs_t    running;
} demo_state_t;

/** A small conveyor interlock, written against the runtime's POU signature. */
void demo_program(plc_pou_ctx_t *ctx);

#endif /* SOFTPLC_DEMO_PROGRAM_H */
