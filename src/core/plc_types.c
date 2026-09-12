/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/plc_types.h"
#include "softplc/plc_status.h"

static const struct { const char *name; size_t bits; } kTypes[PLC_TYPE__COUNT] = {
    [PLC_TYPE_BOOL]  = { "BOOL",   1 },
    [PLC_TYPE_SINT]  = { "SINT",   8 },
    [PLC_TYPE_INT]   = { "INT",   16 },
    [PLC_TYPE_DINT]  = { "DINT",  32 },
    [PLC_TYPE_LINT]  = { "LINT",  64 },
    [PLC_TYPE_USINT] = { "USINT",  8 },
    [PLC_TYPE_UINT]  = { "UINT",  16 },
    [PLC_TYPE_UDINT] = { "UDINT", 32 },
    [PLC_TYPE_ULINT] = { "ULINT", 64 },
    [PLC_TYPE_REAL]  = { "REAL",  32 },
    [PLC_TYPE_LREAL] = { "LREAL", 64 },
    [PLC_TYPE_BYTE]  = { "BYTE",   8 },
    [PLC_TYPE_WORD]  = { "WORD",  16 },
    [PLC_TYPE_DWORD] = { "DWORD", 32 },
    [PLC_TYPE_LWORD] = { "LWORD", 64 },
    [PLC_TYPE_TIME]  = { "TIME",  64 },
};

size_t plc_type_bits(plc_type_id_t id) {
    return (id >= 0 && id < PLC_TYPE__COUNT) ? kTypes[id].bits : 0;
}

const char *plc_type_name(plc_type_id_t id) {
    return (id >= 0 && id < PLC_TYPE__COUNT) ? kTypes[id].name : "<invalid>";
}

const char *plc_strerror(plc_status_t st) {
    switch (st) {
        case PLC_OK:              return "ok";
        case PLC_ERR_INVAL:       return "invalid argument";
        case PLC_ERR_NOMEM:       return "out of memory";
        case PLC_ERR_IO:          return "transport error";
        case PLC_ERR_TIMEOUT:     return "timeout";
        case PLC_ERR_PROTO:       return "protocol or ABI mismatch";
        case PLC_ERR_STATE:       return "invalid state";
        case PLC_ERR_UNSUPPORTED: return "unsupported";
        case PLC_ERR_AGAIN:       return "would block";
        case PLC_ERR_NOTFOUND:    return "not found";
    }
    return "unknown error";
}
