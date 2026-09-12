/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_demo_program.c
 * @brief The ladder logic itself, driven through the real scan engine.
 *
 * The standard function blocks are covered individually by test_std_fb, but
 * that says nothing about a program built from them - and a program is what a
 * plant actually runs. This drives the conveyor interlock the way a scan does,
 * over sequences of inputs, and checks the outputs a ladder engineer would
 * check on the panel.
 *
 * It runs through plc_runtime_scan_once() rather than calling the POU
 * directly, so the wiring is under test too: %I is what the POU reads, %Q is
 * what it writes, and the engine hands it the elapsed time.
 *
 *   %IX0.0 START      %QX0.0 MOTOR
 *   %IX0.1 STOP (NC)  %QX0.1 LAMP
 *   %IX0.2 PART_SENSE %QW2   PART_COUNT
 */
#include "demo_program.h"
#include "softplc/plc_runtime.h"

#include <time.h>
#include "test_util.h"

/* The engine measures dt from the wall clock, so a test that wants exact
 * timer behaviour has to let real time pass. 5 ms a scan keeps the whole file
 * well under a second while still resolving the 200 ms and 500 ms timers. */
#define SCAN_MS 5

static plc_runtime_t  *g_rt;
static demo_state_t    g_demo;
static plc_process_image_t *g_pi;

static void set_inputs(PLC_BOOL start, PLC_BOOL stop_nc, PLC_BOOL part) {
    plc_pi_set_bit(g_pi, PLC_AREA_I, 0, 0, start);
    plc_pi_set_bit(g_pi, PLC_AREA_I, 0, 1, stop_nc);
    plc_pi_set_bit(g_pi, PLC_AREA_I, 0, 2, part);
}

static PLC_BOOL motor(void) { return plc_pi_get_bit(g_pi, PLC_AREA_Q, 0, 0); }
static PLC_BOOL lamp(void)  { return plc_pi_get_bit(g_pi, PLC_AREA_Q, 0, 1); }
static PLC_WORD count(void) { return plc_pi_get_word(g_pi, PLC_AREA_Q, 2); }

/** One scan, with SCAN_MS of real time elapsing first so the timers advance. */
static void scan(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = SCAN_MS * 1000L * 1000L };
    nanosleep(&ts, NULL);
    plc_runtime_scan_once(g_rt);
}

static void scan_for_ms(int ms) {
    for (int i = 0; i < ms / SCAN_MS; ++i) scan();
}

static void setup(void) {
    memset(&g_demo, 0, sizeof(g_demo));
    plc_runtime_config_t rc;
    plc_runtime_config_init(&rc);
    rc.cycle_us = 0;              /* we pace it ourselves */
    rc.i_bytes = rc.q_bytes = 16;
    g_rt = plc_runtime_create(&rc);
    g_pi = plc_runtime_image(g_rt);
    plc_runtime_add_program(g_rt, "demo", demo_program, &g_demo);
    /* STOP is normally closed, so "not pressed" is a closed circuit = 1. */
    set_inputs(false, true, false);
    scan();
}

static void teardown(void) { plc_runtime_destroy(g_rt); g_rt = NULL; }

/* START must be held for the full 500 ms before the motor runs. A start
 * button that energises a conveyor on contact is the classic ladder bug. */
static void test_start_delay(void) {
    setup();
    set_inputs(true, true, false);

    scan_for_ms(300);
    CHECK(!motor());              /* still inside the delay */

    scan_for_ms(150);
    CHECK(!motor());              /* 450 ms: still not yet */

    scan_for_ms(150);
    CHECK(motor());               /* past 500 ms */
    teardown();
}

/* STOP drops the motor in the same scan, and the latch keeps it down after
 * the button is released - it must not restart on its own. */
static void test_stop_is_immediate_and_latched(void) {
    setup();
    set_inputs(true, true, false);
    scan_for_ms(600);
    CHECK(motor());

    set_inputs(true, false, false);   /* STOP pressed: NC contact opens */
    scan();
    CHECK(!motor());

    /* STOP released, START still held. The motor must NOT come back: this is
     * the whole point of a latched stop. Driving the latch from the start
     * delay's *level* instead of its edge fails here, and that failure is a
     * conveyor restarting under an operator's hands. */
    set_inputs(true, true, false);
    scan();
    CHECK(!motor());
    scan_for_ms(600);
    CHECK(!motor());                  /* still down after a full delay period */

    /* A deliberate restart: release START (which resets the delay), hold it
     * again for the full 500 ms. */
    set_inputs(false, true, false);
    scan();
    set_inputs(true, true, false);
    scan_for_ms(300);
    CHECK(!motor());                  /* delay not served yet */
    scan_for_ms(300);
    CHECK(motor());
    teardown();
}

/* A broken STOP wire reads as 0 on a normally-closed contact, which the code
 * claims means "stop". Worth an explicit test: this is the difference between
 * a safe failure and a conveyor that cannot be stopped. */
static void test_broken_stop_wire_stops(void) {
    setup();
    set_inputs(true, true, false);
    scan_for_ms(600);
    CHECK(motor());

    set_inputs(true, false, false);   /* wire break: input reads 0 */
    scan();
    CHECK(!motor());
    teardown();
}

/* One count per part, on the edge - not one per scan the sensor is covered. */
static void test_part_counting_is_edge_driven(void) {
    setup();
    CHECK_EQ_INT(count(), 0);

    set_inputs(false, true, true);    /* part arrives */
    scan();
    CHECK_EQ_INT(count(), 1);

    for (int i = 0; i < 10; ++i) scan();   /* sensor stays covered */
    CHECK_EQ_INT(count(), 1);              /* still one part */

    set_inputs(false, true, false);
    scan();
    set_inputs(false, true, true);         /* second part */
    scan();
    CHECK_EQ_INT(count(), 2);
    teardown();
}

/* The lamp pulses for 200 ms per part and is not retriggerable while lit. */
static void test_lamp_pulse(void) {
    setup();
    set_inputs(false, true, true);
    scan();
    CHECK(lamp());

    scan_for_ms(100);
    CHECK(lamp());                    /* still inside the pulse */

    scan_for_ms(150);
    CHECK(!lamp());                   /* past 200 ms */
    teardown();
}

/* STOP clears the part count - the reset wiring, easy to get backwards. */
static void test_stop_resets_the_count(void) {
    setup();
    for (int i = 0; i < 3; ++i) {
        set_inputs(false, true, true);  scan();
        set_inputs(false, true, false); scan();
    }
    CHECK_EQ_INT(count(), 3);

    set_inputs(false, false, false);  /* STOP */
    scan();
    CHECK_EQ_INT(count(), 0);
    teardown();
}

/* Everything off at power-up: no motor, no lamp, no count. A program that
 * energises anything on its first scan is a program that energises it on
 * every restart. */
static void test_safe_at_first_scan(void) {
    setup();                          /* setup() already ran one scan */
    CHECK(!motor());
    CHECK(!lamp());
    CHECK_EQ_INT(count(), 0);
    teardown();
}

int main(void) {
    test_safe_at_first_scan();
    test_start_delay();
    test_stop_is_immediate_and_latched();
    test_broken_stop_wire_stops();
    test_part_counting_is_edge_driven();
    test_lamp_pulse();
    test_stop_resets_the_count();
    TEST_REPORT("demo_program");
}
