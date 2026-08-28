/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/protocol_adapter.h"

#include <string.h>

void plc_adapter_config_init(plc_adapter_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    /* Everything zero means "let the adapter decide", which is what we want
     * for the sizes and the timeouts.  The one field with no safe zero is the
     * failsafe policy, and HOLD is the conservative pick: keeping the last
     * good image cannot invent an edge that never happened, whereas CLEAR
     * can.  Plants that need CLEAR must say so. */
    cfg->failsafe_policy = PLC_FAILSAFE_HOLD;
}

const char *plc_adapter_state_name(plc_adapter_state_t s) {
    switch (s) {
        case PLC_ADAPTER_CLOSED:   return "CLOSED";
        case PLC_ADAPTER_OPENING:  return "OPENING";
        case PLC_ADAPTER_ONLINE:   return "ONLINE";
        case PLC_ADAPTER_DEGRADED: return "DEGRADED";
        case PLC_ADAPTER_FAULTED:  return "FAULTED";
    }
    return "?";
}

void plc_adapter_apply_failsafe(plc_failsafe_policy_t policy,
                                void *in, const void *last_good, size_t len) {
    if (!in || len == 0) return;
    if (policy == PLC_FAILSAFE_CLEAR || !last_good) {
        memset(in, 0, len);
    } else {
        memcpy(in, last_good, len);
    }
}

static int usable(const plc_protocol_adapter_t *a) {
    return a && a->vtbl;
}

plc_status_t plc_adapter_open(plc_protocol_adapter_t *a,
                              const plc_adapter_config_t *cfg) {
    if (!usable(a) || !a->vtbl->open || !cfg) return PLC_ERR_INVAL;
    return a->vtbl->open(a, cfg);
}

plc_status_t plc_adapter_close(plc_protocol_adapter_t *a) {
    if (!usable(a) || !a->vtbl->close) return PLC_ERR_INVAL;
    return a->vtbl->close(a);
}

plc_status_t plc_adapter_exchange(plc_protocol_adapter_t *a,
                                  const void *out, size_t out_len,
                                  void *in, size_t in_len) {
    if (!usable(a) || !a->vtbl->exchange) return PLC_ERR_INVAL;
    if ((out_len && !out) || (in_len && !in)) return PLC_ERR_INVAL;
    return a->vtbl->exchange(a, out, out_len, in, in_len);
}

plc_status_t plc_adapter_get_caps(const plc_protocol_adapter_t *a,
                                  plc_adapter_caps_t *out) {
    if (!usable(a) || !a->vtbl->get_caps || !out) return PLC_ERR_INVAL;
    return a->vtbl->get_caps(a, out);
}

plc_status_t plc_adapter_get_stats(const plc_protocol_adapter_t *a,
                                   plc_adapter_stats_t *out) {
    if (!usable(a) || !a->vtbl->get_stats || !out) return PLC_ERR_INVAL;
    return a->vtbl->get_stats(a, out);
}

plc_adapter_state_t plc_adapter_state(const plc_protocol_adapter_t *a) {
    if (!usable(a) || !a->vtbl->state) return PLC_ADAPTER_CLOSED;
    return a->vtbl->state(a);
}
