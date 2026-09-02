/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/process_image.h"

#include <stdlib.h>
#include <string.h>

static int area_ok(const plc_process_image_t *pi, plc_area_t a) {
    return pi && a >= 0 && a < PLC_AREA__COUNT && pi->area[a];
}

/* Every accessor funnels through this: a range failure yields a NULL base and
 * the caller degrades to a zero read or a dropped write. */
static uint8_t *slot(const plc_process_image_t *pi, plc_area_t a,
                     size_t off, size_t len) {
    if (!area_ok(pi, a)) return NULL;
    if (off > pi->size[a] || len > pi->size[a] - off) return NULL;
    return pi->area[a] + off;
}

plc_status_t plc_pi_init(plc_process_image_t *pi,
                         size_t i_bytes, size_t q_bytes, size_t m_bytes) {
    if (!pi) return PLC_ERR_INVAL;
    memset(pi, 0, sizeof(*pi));
    /* Claimed before the first allocation, not after the last: the failure
     * path below hands the half-built image to plc_pi_free(), which only frees
     * what it is told it owns. */
    pi->owns_memory = 1;

    const size_t want[PLC_AREA__COUNT] = { i_bytes, q_bytes, m_bytes };
    for (int a = 0; a < PLC_AREA__COUNT; ++a) {
        /* Always allocate at least one byte so that a zero-sized area still
         * has a distinguishable, non-NULL base pointer. */
        uint8_t *p = calloc(want[a] ? want[a] : 1, 1);
        if (!p) { plc_pi_free(pi); return PLC_ERR_NOMEM; }
        pi->area[a] = p;
        pi->size[a] = want[a];
    }
    return PLC_OK;
}

void plc_pi_free(plc_process_image_t *pi) {
    if (!pi) return;
    if (pi->owns_memory) {
        for (int a = 0; a < PLC_AREA__COUNT; ++a) free(pi->area[a]);
    }
    memset(pi, 0, sizeof(*pi));
}

plc_status_t plc_pi_clear(plc_process_image_t *pi, plc_area_t a) {
    if (!area_ok(pi, a)) return PLC_ERR_INVAL;
    memset(pi->area[a], 0, pi->size[a]);
    return PLC_OK;
}

uint8_t *plc_pi_area(const plc_process_image_t *pi, plc_area_t a) {
    return area_ok(pi, a) ? pi->area[a] : NULL;
}

size_t plc_pi_area_size(const plc_process_image_t *pi, plc_area_t a) {
    return area_ok(pi, a) ? pi->size[a] : 0;
}

PLC_BOOL plc_pi_get_bit(const plc_process_image_t *pi, plc_area_t a,
                        size_t byte, unsigned bit) {
    const uint8_t *p = slot(pi, a, byte, 1);
    if (!p || bit > 7) return false;
    return (*p >> bit) & 1u;
}

void plc_pi_set_bit(plc_process_image_t *pi, plc_area_t a,
                    size_t byte, unsigned bit, PLC_BOOL v) {
    uint8_t *p = slot(pi, a, byte, 1);
    if (!p || bit > 7) return;
    if (v) *p |= (uint8_t)(1u << bit);
    else   *p &= (uint8_t)~(1u << bit);
}

PLC_BYTE plc_pi_get_byte(const plc_process_image_t *pi, plc_area_t a, size_t off) {
    const uint8_t *p = slot(pi, a, off, 1);
    return p ? *p : 0;
}

void plc_pi_set_byte(plc_process_image_t *pi, plc_area_t a, size_t off, PLC_BYTE v) {
    uint8_t *p = slot(pi, a, off, 1);
    if (p) *p = v;
}

/* Little-endian on purpose: adapters normalise to LE before handing the image
 * over, so the core has exactly one byte order to reason about. */
PLC_WORD plc_pi_get_word(const plc_process_image_t *pi, plc_area_t a, size_t off) {
    const uint8_t *p = slot(pi, a, off, 2);
    return p ? (PLC_WORD)((PLC_WORD)p[0] | ((PLC_WORD)p[1] << 8)) : 0;
}

void plc_pi_set_word(plc_process_image_t *pi, plc_area_t a, size_t off, PLC_WORD v) {
    uint8_t *p = slot(pi, a, off, 2);
    if (!p) return;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

PLC_DWORD plc_pi_get_dword(const plc_process_image_t *pi, plc_area_t a, size_t off) {
    const uint8_t *p = slot(pi, a, off, 4);
    if (!p) return 0;
    return (PLC_DWORD)p[0] | ((PLC_DWORD)p[1] << 8) |
           ((PLC_DWORD)p[2] << 16) | ((PLC_DWORD)p[3] << 24);
}

void plc_pi_set_dword(plc_process_image_t *pi, plc_area_t a, size_t off, PLC_DWORD v) {
    uint8_t *p = slot(pi, a, off, 4);
    if (!p) return;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

PLC_REAL plc_pi_get_real(const plc_process_image_t *pi, plc_area_t a, size_t off) {
    PLC_DWORD bits = plc_pi_get_dword(pi, a, off);
    PLC_REAL out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

void plc_pi_set_real(plc_process_image_t *pi, plc_area_t a, size_t off, PLC_REAL v) {
    PLC_DWORD bits;
    memcpy(&bits, &v, sizeof(bits));
    plc_pi_set_dword(pi, a, off, bits);
}

plc_status_t plc_pi_write(plc_process_image_t *pi, plc_area_t a, size_t off,
                          const void *src, size_t len) {
    uint8_t *p = slot(pi, a, off, len);
    if (!p || !src) return PLC_ERR_INVAL;
    memcpy(p, src, len);
    return PLC_OK;
}

plc_status_t plc_pi_read(const plc_process_image_t *pi, plc_area_t a, size_t off,
                         void *dst, size_t len) {
    const uint8_t *p = slot(pi, a, off, len);
    if (!p || !dst) return PLC_ERR_INVAL;
    memcpy(dst, p, len);
    return PLC_OK;
}
