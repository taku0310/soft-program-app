/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file demo_program.c
 * @brief A worked example of a POU against this runtime.
 *
 * Conveyor interlock, in the shape a 61131-3 program would have if it were
 * compiled from ST rather than written by hand:
 *
 *     %IX0.0  START      %QX0.0  MOTOR
 *     %IX0.1  STOP (NC)  %QX0.1  LAMP
 *     %IX0.2  PART_SENSE %QW2    PART_COUNT
 *
 * The motor needs START held for 500 ms before it runs, STOP drops it
 * immediately and it stays down until START is deliberately re-actuated, each
 * part edge pulses the lamp for 200 ms and bumps the count.
 *
 * Worth reading for what it shows about the I/O path: this code only ever
 * touches the process image.  It has no idea whether %IX0.2 arrives over
 * EtherNet/IP, over Modbus, or from the loopback adapter in a unit test, and
 * it does not change when the answer changes.
 */
#include "demo_program.h"

void demo_program(plc_pou_ctx_t *ctx) {
    demo_state_t *st = ctx->user;
    if (!st) return;

    const PLC_BOOL start      = plc_pi_get_bit(ctx->pi, PLC_AREA_I, 0, 0);
    const PLC_BOOL stop_nc    = plc_pi_get_bit(ctx->pi, PLC_AREA_I, 0, 1);
    const PLC_BOOL part_sense = plc_pi_get_bit(ctx->pi, PLC_AREA_I, 0, 2);

    /* STOP is wired normally-closed, so a broken wire reads as "stop". */
    const PLC_BOOL stop_pressed = (PLC_BOOL)!stop_nc;

    st->start_delay.IN = start;
    st->start_delay.PT = PLC_TIME_MS(500);
    plc_ton_run(&st->start_delay, ctx->dt);

    /* Set the latch on the *edge* of the delay completing, not on its level.
     *
     * Driving S from start_delay.Q directly restarts the motor the moment
     * STOP is released, because a held START keeps that output true - the
     * operator never re-actuates anything. A stop must require a deliberate
     * restart, so the set has to be a one-shot: START must be released (which
     * resets the TON) and held again to produce another edge. */
    st->start_edge.CLK = st->start_delay.Q;
    plc_r_trig_run(&st->start_edge);

    st->running.S  = st->start_edge.Q;
    st->running.R1 = stop_pressed;
    plc_rs_run(&st->running);

    st->part_edge.CLK = part_sense;
    plc_r_trig_run(&st->part_edge);

    st->lamp_pulse.IN = st->part_edge.Q;
    st->lamp_pulse.PT = PLC_TIME_MS(200);
    plc_tp_run(&st->lamp_pulse, ctx->dt);

    st->part_counter.CU = st->part_edge.Q;
    st->part_counter.R  = stop_pressed;
    st->part_counter.PV = 1000;
    plc_ctu_run(&st->part_counter);

    plc_pi_set_bit(ctx->pi, PLC_AREA_Q, 0, 0, st->running.Q1);
    plc_pi_set_bit(ctx->pi, PLC_AREA_Q, 0, 1, st->lamp_pulse.Q);
    plc_pi_set_word(ctx->pi, PLC_AREA_Q, 2, (PLC_WORD)st->part_counter.CV);
}
