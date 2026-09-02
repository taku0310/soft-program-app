/* SPDX-License-Identifier: Apache-2.0 */
/** Process image accessors, IEC bit addressing, and the range-clamping rule. */
#include "softplc/process_image.h"
#include "test_util.h"

static void test_bits(plc_process_image_t *pi) {
    plc_pi_set_bit(pi, PLC_AREA_Q, 0, 0, true);
    plc_pi_set_bit(pi, PLC_AREA_Q, 0, 7, true);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_Q, 0), 0x81);

    plc_pi_set_bit(pi, PLC_AREA_Q, 0, 0, false);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_Q, 0), 0x80);
    CHECK(plc_pi_get_bit(pi, PLC_AREA_Q, 0, 7));
    CHECK(!plc_pi_get_bit(pi, PLC_AREA_Q, 0, 0));

    /* Bit index above 7 is not an address in %QX<byte>.<bit> at all. */
    plc_pi_set_bit(pi, PLC_AREA_Q, 0, 8, true);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_Q, 0), 0x80);
}

static void test_words(plc_process_image_t *pi) {
    plc_pi_set_word(pi, PLC_AREA_M, 4, 0xBEEF);
    CHECK_EQ_INT(plc_pi_get_word(pi, PLC_AREA_M, 4), 0xBEEF);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_M, 4), 0xEF);  /* little-endian */
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_M, 5), 0xBE);

    plc_pi_set_dword(pi, PLC_AREA_M, 8, 0x01020304u);
    CHECK_EQ_INT(plc_pi_get_dword(pi, PLC_AREA_M, 8), 0x01020304u);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_M, 8), 0x04);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_M, 11), 0x01);
}

static void test_real_roundtrip(plc_process_image_t *pi) {
    plc_pi_set_real(pi, PLC_AREA_M, 16, 3.5f);
    CHECK(plc_pi_get_real(pi, PLC_AREA_M, 16) == 3.5f);
    plc_pi_set_real(pi, PLC_AREA_M, 16, -0.125f);
    CHECK(plc_pi_get_real(pi, PLC_AREA_M, 16) == -0.125f);
}

/* A POU that addresses past the end of an area must not be able to corrupt the
 * runtime; it reads zero and its writes go nowhere. */
static void test_out_of_range_is_contained(plc_process_image_t *pi) {
    const size_t q = plc_pi_area_size(pi, PLC_AREA_Q);

    plc_pi_set_byte(pi, PLC_AREA_Q, q, 0xFF);
    CHECK_EQ_INT(plc_pi_get_byte(pi, PLC_AREA_Q, q), 0);
    CHECK_EQ_INT(plc_pi_get_word(pi, PLC_AREA_Q, q - 1), 0);  /* straddles the end */
    CHECK_EQ_INT(plc_pi_get_dword(pi, PLC_AREA_Q, q - 2), 0);

    uint8_t buf[8];
    CHECK_EQ_INT(plc_pi_read(pi, PLC_AREA_Q, q - 4, buf, sizeof(buf)), PLC_ERR_INVAL);
    CHECK_EQ_INT(plc_pi_write(pi, PLC_AREA_Q, q - 4, buf, sizeof(buf)), PLC_ERR_INVAL);
}

static void test_bulk_and_clear(plc_process_image_t *pi) {
    const uint8_t src[4] = { 1, 2, 3, 4 };
    uint8_t dst[4] = { 0 };

    CHECK_EQ_INT(plc_pi_write(pi, PLC_AREA_I, 2, src, sizeof(src)), PLC_OK);
    CHECK_EQ_INT(plc_pi_read(pi, PLC_AREA_I, 2, dst, sizeof(dst)), PLC_OK);
    CHECK_MEM_EQ(dst, src, sizeof(src));

    CHECK_EQ_INT(plc_pi_clear(pi, PLC_AREA_I), PLC_OK);
    CHECK_EQ_INT(plc_pi_get_dword(pi, PLC_AREA_I, 2), 0);
}

int main(void) {
    plc_process_image_t pi;
    CHECK_EQ_INT(plc_pi_init(&pi, 32, 32, 64), PLC_OK);

    test_bits(&pi);
    test_words(&pi);
    test_real_roundtrip(&pi);
    test_out_of_range_is_contained(&pi);
    test_bulk_and_clear(&pi);

    plc_pi_free(&pi);
    plc_pi_free(&pi);   /* idempotent */

    TEST_REPORT("process_image");
}
