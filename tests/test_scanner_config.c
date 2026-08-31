/* SPDX-License-Identifier: Apache-2.0 */
/** Device table parsing: offsets, validation, and the refusals. */
#include "eip_scanner_config.h"
#include "test_util.h"

static void test_good_table(void) {
    static const char text[] =
        "# ip          cfg  o2t  t2o  o2t_len t2o_len rpi    failsafe\n"
        "192.168.1.10  151  150  100  32      32      10000  hold\n"
        "\n"
        "192.168.1.11  151  150  100  8       16      5000   clear   # a drive\n";

    eip_scanner_config_t cfg;
    char err[160] = "";
    CHECK_EQ_INT(eip_scanner_config_parse(&cfg, text, err, sizeof(err)), PLC_OK);
    CHECK_EQ_INT(cfg.device_count, 2);

    CHECK(strcmp(cfg.devices[0].address, "192.168.1.10") == 0);
    CHECK_EQ_INT(cfg.devices[0].config_assembly, 151);
    CHECK_EQ_INT(cfg.devices[0].o2t_assembly, 150);
    CHECK_EQ_INT(cfg.devices[0].t2o_assembly, 100);
    CHECK_EQ_INT(cfg.devices[0].o2t_rpi_us, 10000);
    CHECK_EQ_INT(cfg.devices[0].failsafe_policy, PLC_FAILSAFE_HOLD);
    CHECK_EQ_INT(cfg.devices[1].failsafe_policy, PLC_FAILSAFE_CLEAR);

    /* Devices pack in table order, so the %I/%Q map reads straight off the
     * file - which is what someone wiring a POU to a drive needs. */
    CHECK_EQ_INT(cfg.devices[0].o2t_offset, 0);
    CHECK_EQ_INT(cfg.devices[0].t2o_offset, 0);
    CHECK_EQ_INT(cfg.devices[1].o2t_offset, 32);
    CHECK_EQ_INT(cfg.devices[1].t2o_offset, 32);

    CHECK_EQ_INT(cfg.total_o2t_bytes, 40);
    CHECK_EQ_INT(cfg.total_t2o_bytes, 48);
}

/* Every field is required. A silently defaulted RPI or image size is a plant
 * fault waiting to happen, so a short row must be refused, not completed. */
static void test_short_row_is_refused(void) {
    eip_scanner_config_t cfg;
    char err[160] = "";
    CHECK_EQ_INT(eip_scanner_config_parse(&cfg, "192.168.1.10 151 150 100\n",
                                          err, sizeof(err)), PLC_ERR_INVAL);
    CHECK(strstr(err, "line 1") != NULL);
}

static void test_bad_fields(void) {
    eip_scanner_config_t cfg;
    char err[160];

    CHECK_EQ_INT(eip_scanner_config_parse(&cfg,
        "10.0.0.1 151 150 100 32 32 10000 maybe\n", err, sizeof(err)),
        PLC_ERR_INVAL);
    CHECK(strstr(err, "failsafe") != NULL);

    /* RPI 0 would mean "as fast as possible", which no scanner should accept
     * by accident. */
    CHECK_EQ_INT(eip_scanner_config_parse(&cfg,
        "10.0.0.1 151 150 100 32 32 0 hold\n", err, sizeof(err)),
        PLC_ERR_INVAL);
    CHECK(strstr(err, "RPI") != NULL);

    CHECK_EQ_INT(eip_scanner_config_parse(&cfg,
        "10.0.0.1 151 150 100 32 32 abc hold\n", err, sizeof(err)),
        PLC_ERR_INVAL);

    /* Trailing junk in a number must not be accepted as the number. */
    CHECK_EQ_INT(eip_scanner_config_parse(&cfg,
        "10.0.0.1 151x 150 100 32 32 10000 hold\n", err, sizeof(err)),
        PLC_ERR_INVAL);
}

static void test_empty_table_is_refused(void) {
    eip_scanner_config_t cfg;
    char err[160] = "";
    CHECK_EQ_INT(eip_scanner_config_parse(&cfg, "# only a comment\n",
                                          err, sizeof(err)), PLC_ERR_INVAL);
    CHECK(strstr(err, "no devices") != NULL);
}

/* The aggregate must fit one IPC frame; exceeding it has to fail at load time
 * rather than truncate an image at run time. */
static void test_aggregate_limit(void) {
    char text[4096] = "";
    for (int i = 0; i < 40; ++i) {
        char row[128];
        snprintf(row, sizeof(row), "10.0.0.%d 151 150 100 32 32 10000 hold\n", i);
        strcat(text, row);
    }
    eip_scanner_config_t cfg;
    char err[160] = "";
    CHECK(eip_scanner_config_parse(&cfg, text, err, sizeof(err)) != PLC_OK);
}

int main(void) {
    test_good_table();
    test_short_row_is_refused();
    test_bad_fields();
    test_empty_table_is_refused();
    test_aggregate_limit();
    TEST_REPORT("scanner_config");
}
