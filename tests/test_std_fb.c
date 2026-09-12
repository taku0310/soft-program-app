/* SPDX-License-Identifier: Apache-2.0 */
/** Standard function block semantics, stepped with an explicit dt so the
 *  results are exact rather than timing-dependent. */
#include "softplc/std_fb.h"
#include "test_util.h"

#define MS PLC_TIME_MS(1)

static void test_r_trig(void) {
    plc_r_trig_t fb = { 0 };
    fb.CLK = false; plc_r_trig_run(&fb); CHECK(!fb.Q);
    fb.CLK = true;  plc_r_trig_run(&fb); CHECK(fb.Q);   /* the edge */
    fb.CLK = true;  plc_r_trig_run(&fb); CHECK(!fb.Q);  /* level, not edge */
    fb.CLK = false; plc_r_trig_run(&fb); CHECK(!fb.Q);
    fb.CLK = true;  plc_r_trig_run(&fb); CHECK(fb.Q);
}

static void test_f_trig(void) {
    plc_f_trig_t fb = { 0 };
    fb.CLK = true;  plc_f_trig_run(&fb); CHECK(!fb.Q);
    fb.CLK = false; plc_f_trig_run(&fb); CHECK(fb.Q);
    fb.CLK = false; plc_f_trig_run(&fb); CHECK(!fb.Q);
}

static void test_bistables(void) {
    plc_rs_t rs = { 0 };
    rs.S = true;  rs.R1 = false; plc_rs_run(&rs); CHECK(rs.Q1);
    rs.S = false; rs.R1 = false; plc_rs_run(&rs); CHECK(rs.Q1);   /* latched */
    rs.S = true;  rs.R1 = true;  plc_rs_run(&rs); CHECK(!rs.Q1);  /* reset wins */

    plc_sr_t sr = { 0 };
    sr.S1 = true; sr.R = true;  plc_sr_run(&sr); CHECK(sr.Q1);    /* set wins */
    sr.S1 = false; sr.R = true; plc_sr_run(&sr); CHECK(!sr.Q1);
}

static void test_ton(void) {
    plc_ton_t t = { 0 };
    t.PT = PLC_TIME_MS(100);

    t.IN = true;
    for (int i = 0; i < 9; ++i) { plc_ton_run(&t, MS * 10); CHECK(!t.Q); }
    plc_ton_run(&t, MS * 10);
    CHECK(t.Q);
    CHECK_EQ_INT(t.ET, PLC_TIME_MS(100));   /* ET stops at PT */

    plc_ton_run(&t, MS * 50);
    CHECK_EQ_INT(t.ET, PLC_TIME_MS(100));   /* and stays there */

    t.IN = false;
    plc_ton_run(&t, MS);
    CHECK(!t.Q);
    CHECK_EQ_INT(t.ET, 0);
}

static void test_tof(void) {
    plc_tof_t t = { 0 };
    t.PT = PLC_TIME_MS(50);

    /* A block whose IN has never been true has never started timing out, so Q
     * must stay false however long the runtime scans.  Deriving "running" from
     * ET < PT would report Q here for the first PT after start-up. */
    for (int i = 0; i < 10; ++i) { plc_tof_run(&t, MS * 20); CHECK(!t.Q); }
    CHECK_EQ_INT(t.ET, 0);

    t.IN = true;  plc_tof_run(&t, MS); CHECK(t.Q);
    t.IN = false;
    plc_tof_run(&t, MS * 20); CHECK(t.Q);   /* still held */
    plc_tof_run(&t, MS * 20); CHECK(t.Q);
    plc_tof_run(&t, MS * 20); CHECK(!t.Q);  /* 60 ms >= 50 ms */
}

static void test_tp(void) {
    plc_tp_t t = { 0 };
    t.PT = PLC_TIME_MS(30);

    t.IN = true;
    plc_tp_run(&t, 0);        CHECK(t.Q);
    t.IN = false;             /* pulse must survive IN dropping */
    plc_tp_run(&t, MS * 10);  CHECK(t.Q);
    plc_tp_run(&t, MS * 10);  CHECK(t.Q);
    plc_tp_run(&t, MS * 10);  CHECK(!t.Q);

    /* Not retriggerable while running. */
    t.IN = true;  plc_tp_run(&t, 0);       CHECK(t.Q);
    t.IN = true;  plc_tp_run(&t, MS * 5);  CHECK(t.Q);
    CHECK_EQ_INT(t.ET, PLC_TIME_MS(5));
}

static void test_counters(void) {
    plc_ctu_t up = { 0 };
    up.PV = 3;
    for (int i = 0; i < 3; ++i) {
        up.CU = true;  plc_ctu_run(&up);
        up.CU = false; plc_ctu_run(&up);
    }
    CHECK_EQ_INT(up.CV, 3);
    CHECK(up.Q);
    up.R = true; plc_ctu_run(&up);
    CHECK_EQ_INT(up.CV, 0);
    CHECK(!up.Q);

    plc_ctd_t down = { 0 };
    down.PV = 2;
    down.LD = true;  plc_ctd_run(&down); CHECK_EQ_INT(down.CV, 2);
    down.LD = false;
    down.CD = true;  plc_ctd_run(&down); CHECK_EQ_INT(down.CV, 1);
    down.CD = false; plc_ctd_run(&down);
    down.CD = true;  plc_ctd_run(&down); CHECK_EQ_INT(down.CV, 0);
    CHECK(down.Q);

    plc_ctud_t ud = { 0 };
    ud.PV = 2;
    ud.CU = true; ud.CD = true; plc_ctud_run(&ud);
    CHECK_EQ_INT(ud.CV, 0);       /* simultaneous edges cancel */
    ud.CU = false; ud.CD = false; plc_ctud_run(&ud);
    ud.CU = true;  plc_ctud_run(&ud);
    CHECK_EQ_INT(ud.CV, 1);
}

int main(void) {
    test_r_trig();
    test_f_trig();
    test_bistables();
    test_ton();
    test_tof();
    test_tp();
    test_counters();
    TEST_REPORT("std_fb");
}
