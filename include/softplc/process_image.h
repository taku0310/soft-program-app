/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file process_image.h
 * @brief IEC 61131-3 process image: %I (inputs), %Q (outputs), %M (marker).
 *
 * The image is plain bytes owned by the core.  Adapters never address it; the
 * I/O map copies adapter images in and out of it at defined points in the
 * scan, which is what keeps memory model B (copy, never share) true all the
 * way from the fieldbus to the POU.
 *
 * Bit numbering follows IEC 61131-3 %IX<byte>.<bit>, bit 0 = LSB.  Multi-byte
 * accessors are little-endian on the wire-facing side, which matches
 * EtherNet/IP and Modbus/TCP register order after the adapter has normalised.
 */
#ifndef SOFTPLC_PROCESS_IMAGE_H
#define SOFTPLC_PROCESS_IMAGE_H

#include "softplc/plc_status.h"
#include "softplc/plc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum plc_area {
    PLC_AREA_I = 0, /**< %I - inputs, written by the I/O map, read by POUs  */
    PLC_AREA_Q,     /**< %Q - outputs, written by POUs, read by the I/O map */
    /** %M - marker area, POU scratch.  NOT retained: this is plain RAM and
     *  its contents are lost on restart.  IEC 61131-3 RETAIN/PERSISTENT
     *  semantics would need backing storage, which is not implemented. */
    PLC_AREA_M,
    PLC_AREA__COUNT
} plc_area_t;

typedef struct plc_process_image {
    uint8_t *area[PLC_AREA__COUNT];
    size_t   size[PLC_AREA__COUNT];
    int      owns_memory;
} plc_process_image_t;

/** Allocate and zero the three areas. */
plc_status_t plc_pi_init(plc_process_image_t *pi,
                         size_t i_bytes, size_t q_bytes, size_t m_bytes);

/** Free anything plc_pi_init() allocated.  Idempotent. */
void plc_pi_free(plc_process_image_t *pi);

/** Zero one area. */
plc_status_t plc_pi_clear(plc_process_image_t *pi, plc_area_t area);

/** Base pointer of @p area, or NULL.  For the I/O map only. */
uint8_t *plc_pi_area(const plc_process_image_t *pi, plc_area_t area);
size_t   plc_pi_area_size(const plc_process_image_t *pi, plc_area_t area);

/*
 * Accessors.  Every one is range-checked: an out-of-range read yields 0 and an
 * out-of-range write is dropped.  A POU that walks off the end of the image
 * must not be able to corrupt the runtime, and on a control system silently
 * clamping beats aborting the scan.
 */
PLC_BOOL  plc_pi_get_bit  (const plc_process_image_t *pi, plc_area_t a, size_t byte, unsigned bit);
void      plc_pi_set_bit  (plc_process_image_t *pi, plc_area_t a, size_t byte, unsigned bit, PLC_BOOL v);

PLC_BYTE  plc_pi_get_byte (const plc_process_image_t *pi, plc_area_t a, size_t off);
void      plc_pi_set_byte (plc_process_image_t *pi, plc_area_t a, size_t off, PLC_BYTE v);

PLC_WORD  plc_pi_get_word (const plc_process_image_t *pi, plc_area_t a, size_t off);
void      plc_pi_set_word (plc_process_image_t *pi, plc_area_t a, size_t off, PLC_WORD v);

PLC_DWORD plc_pi_get_dword(const plc_process_image_t *pi, plc_area_t a, size_t off);
void      plc_pi_set_dword(plc_process_image_t *pi, plc_area_t a, size_t off, PLC_DWORD v);

PLC_REAL  plc_pi_get_real (const plc_process_image_t *pi, plc_area_t a, size_t off);
void      plc_pi_set_real (plc_process_image_t *pi, plc_area_t a, size_t off, PLC_REAL v);

/** Bulk copy into @p area at @p off.  Short-writes are rejected, not clamped. */
plc_status_t plc_pi_write(plc_process_image_t *pi, plc_area_t a, size_t off,
                          const void *src, size_t len);
/** Bulk copy out of @p area at @p off. */
plc_status_t plc_pi_read(const plc_process_image_t *pi, plc_area_t a, size_t off,
                         void *dst, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PROCESS_IMAGE_H */
