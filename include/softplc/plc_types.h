/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plc_types.h
 * @brief IEC 61131-3 elementary data types and their C mapping.
 *
 * The runtime keeps the IEC 61131-3 type system as the contract seen by
 * application code (POUs).  Protocol adapters never see these types: they
 * exchange opaque byte images, which the I/O mapping layer projects onto the
 * process image.
 */
#ifndef SOFTPLC_PLC_TYPES_H
#define SOFTPLC_PLC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- IEC 61131-3 elementary types (Table 10) ----------------------------- */

typedef bool     PLC_BOOL;

typedef int8_t   PLC_SINT;
typedef int16_t  PLC_INT;
typedef int32_t  PLC_DINT;
typedef int64_t  PLC_LINT;

typedef uint8_t  PLC_USINT;
typedef uint16_t PLC_UINT;
typedef uint32_t PLC_UDINT;
typedef uint64_t PLC_ULINT;

typedef float    PLC_REAL;
typedef double   PLC_LREAL;

typedef uint8_t  PLC_BYTE;
typedef uint16_t PLC_WORD;
typedef uint32_t PLC_DWORD;
typedef uint64_t PLC_LWORD;

/** IEC duration.  Stored in nanoseconds so that TIME arithmetic never loses
 *  resolution against the runtime clock, which is CLOCK_MONOTONIC. */
typedef int64_t  PLC_TIME;

#define PLC_TIME_NS(v)  ((PLC_TIME)(v))
#define PLC_TIME_US(v)  ((PLC_TIME)(v) * 1000)
#define PLC_TIME_MS(v)  ((PLC_TIME)(v) * 1000000)
#define PLC_TIME_S(v)   ((PLC_TIME)(v) * 1000000000)

/** Elementary type tags, used by the I/O mapping table and by tooling. */
typedef enum plc_type_id {
    PLC_TYPE_BOOL = 0,
    PLC_TYPE_SINT,
    PLC_TYPE_INT,
    PLC_TYPE_DINT,
    PLC_TYPE_LINT,
    PLC_TYPE_USINT,
    PLC_TYPE_UINT,
    PLC_TYPE_UDINT,
    PLC_TYPE_ULINT,
    PLC_TYPE_REAL,
    PLC_TYPE_LREAL,
    PLC_TYPE_BYTE,
    PLC_TYPE_WORD,
    PLC_TYPE_DWORD,
    PLC_TYPE_LWORD,
    PLC_TYPE_TIME,
    PLC_TYPE__COUNT
} plc_type_id_t;

/** Width in bits of an elementary type (BOOL reports 1). */
size_t plc_type_bits(plc_type_id_t id);

/** Human readable IEC name, e.g. "UDINT".  Never NULL. */
const char *plc_type_name(plc_type_id_t id);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PLC_TYPES_H */
