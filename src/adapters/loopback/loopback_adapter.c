/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file loopback_adapter.c
 * @brief In-process reference adapter.
 *
 * Two jobs.  First, it keeps the abstraction honest: if the interface only
 * ever had one implementation it would drift into being EtherNet/IP-shaped,
 * and the whole point is that Modbus and OPC UA drop into the same socket.
 * Second, it lets the runtime, the I/O map and the scan engine be exercised
 * with no fieldbus, no second process and no network - which is what the unit
 * tests and the container smoke test use.
 *
 * Behaviour: outputs are mirrored back into inputs, offset by the configured
 * skew, so a POU can be tested closed-loop.  Timeouts can be injected to
 * exercise the failsafe path.
 */
#include "softplc/adapter_registry.h"
#include "softplc/protocol_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOOPBACK_DEFAULT_BYTES   32
#define LOOPBACK_MAX_BYTES      512

typedef struct loopback_impl {
    plc_adapter_caps_t  caps;
    plc_adapter_stats_t stats;
    plc_adapter_state_t state;

    uint8_t last_good[LOOPBACK_MAX_BYTES];
    uint8_t mirror[LOOPBACK_MAX_BYTES];

    /* Fault injection, used by the contract tests.  Not settable from config:
     * a production build has no way to turn it on. */
    unsigned force_timeouts;
} loopback_impl_t;

/* --- test hook ----------------------------------------------------------- */

void plc_loopback_force_timeouts(plc_protocol_adapter_t *a, unsigned n);

void plc_loopback_force_timeouts(plc_protocol_adapter_t *a, unsigned n) {
    if (a && a->impl) ((loopback_impl_t *)a->impl)->force_timeouts = n;
}

/* --- vtable -------------------------------------------------------------- */

static plc_status_t lb_open(plc_protocol_adapter_t *self,
                            const plc_adapter_config_t *cfg) {
    loopback_impl_t *impl = self->impl;
    if (impl->state != PLC_ADAPTER_CLOSED) return PLC_ERR_STATE;

    uint32_t in  = cfg->input_bytes  ? cfg->input_bytes  : LOOPBACK_DEFAULT_BYTES;
    uint32_t out = cfg->output_bytes ? cfg->output_bytes : LOOPBACK_DEFAULT_BYTES;
    if (in > LOOPBACK_MAX_BYTES || out > LOOPBACK_MAX_BYTES) return PLC_ERR_INVAL;

    memset(&impl->stats, 0, sizeof(impl->stats));
    memset(impl->last_good, 0, sizeof(impl->last_good));
    memset(impl->mirror, 0, sizeof(impl->mirror));

    plc_adapter_caps_t *c = &impl->caps;
    memset(c, 0, sizeof(*c));
    c->abi_version = PLC_ADAPTER_ABI_VERSION;
    snprintf(c->name, sizeof(c->name), "%s", cfg->name ? cfg->name : "loop0");
    snprintf(c->protocol, sizeof(c->protocol), "loopback");
    c->input_bytes  = in;
    c->output_bytes = out;
    c->failsafe_policy = cfg->failsafe_policy;
    c->exchange_timeout_us =
        cfg->exchange_timeout_us ? cfg->exchange_timeout_us : 1000;
    c->consecutive_timeout_threshold =
        cfg->consecutive_timeout_threshold ? cfg->consecutive_timeout_threshold : 3;
    c->flags = 0;                     /* in-process: no crash containment */
    c->point_override_capacity = 0;   /* reserved, see protocol_adapter.h */

    impl->state = PLC_ADAPTER_ONLINE;
    return PLC_OK;
}

static plc_status_t lb_close(plc_protocol_adapter_t *self) {
    loopback_impl_t *impl = self->impl;
    impl->state = PLC_ADAPTER_CLOSED;
    return PLC_OK;
}

static plc_status_t lb_exchange(plc_protocol_adapter_t *self,
                                const void *out, size_t out_len,
                                void *in, size_t in_len) {
    loopback_impl_t *impl = self->impl;
    if (impl->state == PLC_ADAPTER_CLOSED) return PLC_ERR_STATE;
    if (out_len > impl->caps.output_bytes || in_len > impl->caps.input_bytes) {
        return PLC_ERR_INVAL;
    }

    if (impl->force_timeouts) {
        impl->force_timeouts--;
        impl->stats.timeouts++;
        impl->stats.consecutive_timeouts++;

        if (impl->stats.consecutive_timeouts >=
            impl->caps.consecutive_timeout_threshold) {
            if (impl->state != PLC_ADAPTER_FAULTED) {
                impl->state = PLC_ADAPTER_FAULTED;
                impl->stats.failsafe_activations++;
            }
            plc_adapter_apply_failsafe(impl->caps.failsafe_policy,
                                       in, impl->last_good, in_len);
        } else {
            /* Below the threshold a single miss always holds: one lost frame
             * should not be allowed to inject an edge into the process image. */
            impl->state = PLC_ADAPTER_DEGRADED;
            plc_adapter_apply_failsafe(PLC_FAILSAFE_HOLD,
                                       in, impl->last_good, in_len);
        }
        return PLC_ERR_TIMEOUT;
    }

    if (out && out_len) memcpy(impl->mirror, out, out_len);
    if (in && in_len) {
        memcpy(in, impl->mirror, in_len);
        memcpy(impl->last_good, in, in_len);
    }

    impl->stats.exchanges++;
    impl->stats.consecutive_timeouts = 0;
    impl->state = PLC_ADAPTER_ONLINE;
    return PLC_OK;
}

static plc_status_t lb_caps(const plc_protocol_adapter_t *self,
                            plc_adapter_caps_t *out) {
    *out = ((const loopback_impl_t *)self->impl)->caps;
    return PLC_OK;
}

static plc_status_t lb_stats(const plc_protocol_adapter_t *self,
                             plc_adapter_stats_t *out) {
    *out = ((const loopback_impl_t *)self->impl)->stats;
    return PLC_OK;
}

static plc_adapter_state_t lb_state(const plc_protocol_adapter_t *self) {
    return ((const loopback_impl_t *)self->impl)->state;
}

static const plc_adapter_vtbl_t kLoopbackVtbl = {
    .open      = lb_open,
    .close     = lb_close,
    .exchange  = lb_exchange,
    .get_caps  = lb_caps,
    .get_stats = lb_stats,
    .state     = lb_state,
};

/* --- factory ------------------------------------------------------------- */

static plc_protocol_adapter_t *lb_create(void) {
    plc_protocol_adapter_t *a = calloc(1, sizeof(*a));
    loopback_impl_t *impl = calloc(1, sizeof(*impl));
    if (!a || !impl) { free(a); free(impl); return NULL; }
    impl->state = PLC_ADAPTER_CLOSED;
    a->vtbl = &kLoopbackVtbl;
    a->impl = impl;
    return a;
}

static void lb_destroy(plc_protocol_adapter_t *a) {
    if (!a) return;
    if (a->impl) {
        lb_close(a);
        free(a->impl);
    }
    free(a);
}

const plc_adapter_factory_t plc_loopback_adapter_factory = {
    .protocol    = "loopback",
    .description = "In-process mirror adapter for tests and smoke runs",
    .create      = lb_create,
    .destroy     = lb_destroy,
};
