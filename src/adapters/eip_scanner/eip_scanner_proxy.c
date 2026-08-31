/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_proxy.c
 * @brief PLC-core side of the EtherNet/IP Scanner: a ProtocolAdapter over IPC.
 *
 * Structurally the twin of eip_adapter_proxy.c - same rings, same sequence
 * matching, same "exchange() timeout is the liveness check" rule (ADR 0003),
 * same crash containment. Two things differ, both consequences of the
 * originator role:
 *
 *  - the aggregate image covers N devices, so the failsafe applied *here* is
 *    the coarse one: it fires when the scanner *process* stops answering, and
 *    then the whole image is held or cleared because nothing is known about
 *    any device. Losing a single device while the process is healthy is
 *    handled inside the scanner, per device, and reported through the health
 *    bytes at the head of the input image;
 *
 *  - the input image is health bytes followed by the T->O data. The core does
 *    not interpret either; it copies the block into %I and lets the POU read
 *    the health bytes as ordinary inputs.
 *
 * Direction reminder, because it inverts against the adapter role: here PLC
 * %Q is O->T and PLC %I is T->O. See eip_scanner_shm_layout.h.
 */
#include "eip_scanner_shm_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "softplc/adapter_registry.h"
#include "softplc/ipc/shm.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

typedef struct scanner_proxy {
    plc_adapter_caps_t  caps;
    plc_adapter_stats_t stats;
    plc_adapter_state_t state;

    char               instance[EIP_SCANNER_INSTANCE_MAX];
    plc_shm_t          shm;
    eip_scanner_shm_t *map;
    sem_t             *sem_req;
    sem_t             *sem_rsp;
    char               sem_req_name[EIP_SCANNER_SHM_NAME_MAX];
    char               sem_rsp_name[EIP_SCANNER_SHM_NAME_MAX];

    uint32_t next_seq;
    uint8_t  last_good[PLC_IPC_MAX_FRAME_BYTES];

    plc_ipc_frame_t scratch;
} scanner_proxy_t;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t env_u32(const char *key, uint32_t fallback) {
    const char *v = getenv(key);
    if (!v || !*v) return fallback;
    char *end = NULL;
    const unsigned long n = strtoul(v, &end, 10);
    return (end == v || n == 0 || n > 0xFFFFFFFFul) ? fallback : (uint32_t)n;
}

static const char *env_str(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

/* --- lifecycle ----------------------------------------------------------- */

static void proxy_teardown(scanner_proxy_t *p) {
    plc_sem_close(p->sem_req, p->sem_req_name, 1);
    plc_sem_close(p->sem_rsp, p->sem_rsp_name, 1);
    p->sem_req = p->sem_rsp = NULL;
    plc_shm_close(&p->shm);
    p->map = NULL;
}

static plc_status_t proxy_open(plc_protocol_adapter_t *self,
                               const plc_adapter_config_t *cfg) {
    scanner_proxy_t *p = self->impl;
    if (p->state != PLC_ADAPTER_CLOSED) return PLC_ERR_STATE;

    snprintf(p->instance, sizeof(p->instance), "%s",
             (cfg->endpoint && *cfg->endpoint) ? cfg->endpoint
             : (cfg->name && *cfg->name) ? cfg->name : "default");

    /* The image sizes come from the device table, which the scanner process
     * owns.  The core cannot know them before the peer publishes them, so it
     * reads the same file rather than guessing - a mismatch here would show up
     * as a silently truncated image rather than an error. */
    eip_scanner_config_t table;
    char err[160];
    const char *path = env_str("SOFTPLC_SCANNER_DEVICES",
                               "/etc/softplc/scanner-devices.conf");
    plc_status_t st = eip_scanner_config_load(&table, path, err, sizeof(err));
    if (st != PLC_OK) {
        PLC_LOG_ERR("scanner device table %s: %s", path, err);
        return st;
    }

    const uint32_t health = EIP_SCANNER_HEALTH_BYTES(table.device_count);
    const uint32_t in_bytes  = health + table.total_t2o_bytes;
    const uint32_t out_bytes = table.total_o2t_bytes;
    if (in_bytes > PLC_IPC_MAX_FRAME_BYTES) {
        PLC_LOG_ERR("scanner input image %u exceeds the frame limit %u",
                    in_bytes, (unsigned)PLC_IPC_MAX_FRAME_BYTES);
        return PLC_ERR_INVAL;
    }

    char shm_name[EIP_SCANNER_SHM_NAME_MAX];
    eip_scanner_shm_name(shm_name, sizeof(shm_name), p->instance);
    eip_scanner_sem_req_name(p->sem_req_name, sizeof(p->sem_req_name), p->instance);
    eip_scanner_sem_rsp_name(p->sem_rsp_name, sizeof(p->sem_rsp_name), p->instance);

    st = plc_shm_create(&p->shm, shm_name, sizeof(eip_scanner_shm_t));
    if (st != PLC_OK) return st;
    p->map = p->shm.base;

    p->map->abi_version     = EIP_SCANNER_SHM_ABI_VERSION;
    p->map->layout_bytes    = (uint32_t)sizeof(eip_scanner_shm_t);
    p->map->device_count    = table.device_count;
    p->map->health_bytes    = health;
    p->map->o2t_total_bytes = out_bytes;
    p->map->t2o_total_bytes = table.total_t2o_bytes;
    p->map->input_bytes     = in_bytes;
    plc_spsc_init(&p->map->req);
    plc_spsc_init(&p->map->rsp);
    atomic_store(&p->map->status.adapter_state, PLC_ADAPTER_CLOSED);

    /* Doorbells before the magic word: the peer treats magic as "everything is
     * published" and goes straight to sem_open(), which it does not retry. */
    if ((st = plc_sem_create(&p->sem_req, p->sem_req_name)) != PLC_OK ||
        (st = plc_sem_create(&p->sem_rsp, p->sem_rsp_name)) != PLC_OK) {
        proxy_teardown(p);
        return st;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->map->magic = EIP_SCANNER_SHM_MAGIC;

    plc_adapter_caps_t *c = &p->caps;
    memset(c, 0, sizeof(*c));
    c->abi_version = PLC_ADAPTER_ABI_VERSION;
    snprintf(c->name, sizeof(c->name), "%s", cfg->name ? cfg->name : "eipscan0");
    snprintf(c->protocol, sizeof(c->protocol), "ethernet-ip-scanner");
    c->input_bytes  = in_bytes;
    c->output_bytes = out_bytes;
    /* This is the process-level policy.  Per-device policy lives in the device
     * table and is applied by the scanner itself. */
    c->failsafe_policy = cfg->failsafe_policy;
    c->exchange_timeout_us = cfg->exchange_timeout_us
        ? cfg->exchange_timeout_us
        : env_u32("SOFTPLC_SCANNER_EXCHANGE_TIMEOUT_US",
                  EIP_SCANNER_DEFAULT_EXCHANGE_TIMEOUT_US);
    c->consecutive_timeout_threshold = cfg->consecutive_timeout_threshold
        ? cfg->consecutive_timeout_threshold
        : env_u32("SOFTPLC_SCANNER_TIMEOUT_THRESHOLD",
                  EIP_SCANNER_DEFAULT_TIMEOUT_THRESHOLD);
    c->flags = PLC_ADAPTER_CAP_OUT_OF_PROCESS;
    c->point_override_capacity = 0;

    memset(&p->stats, 0, sizeof(p->stats));
    memset(p->last_good, 0, sizeof(p->last_good));
    p->next_seq = 1;
    p->state    = PLC_ADAPTER_OPENING;

    PLC_LOG_INFO("eip-scanner '%s' ready on %s: %u device(s), "
                 "%%I=%u (%u health + %u data) %%Q=%u",
                 c->name, shm_name, table.device_count,
                 in_bytes, health, table.total_t2o_bytes, out_bytes);
    for (uint32_t i = 0; i < table.device_count; ++i) {
        const eip_scanner_device_t *d = &table.devices[i];
        PLC_LOG_INFO("  device %u: %s cfg=%u o2t=%u t2o=%u "
                     "%%Q[+%u..%u) %%I[+%u..%u) rpi=%uus failsafe=%s",
                     i, d->address, d->config_assembly,
                     d->o2t_assembly, d->t2o_assembly,
                     d->o2t_offset, d->o2t_offset + d->o2t_bytes,
                     health + d->t2o_offset, health + d->t2o_offset + d->t2o_bytes,
                     d->o2t_rpi_us,
                     d->failsafe_policy == PLC_FAILSAFE_CLEAR ? "CLEAR" : "HOLD");
    }
    return PLC_OK;
}

static plc_status_t proxy_close(plc_protocol_adapter_t *self) {
    scanner_proxy_t *p = self->impl;
    if (p->state == PLC_ADAPTER_CLOSED) return PLC_OK;
    proxy_teardown(p);
    p->state = PLC_ADAPTER_CLOSED;
    return PLC_OK;
}

/* --- the cyclic exchange -------------------------------------------------- */

/**
 * The scanner process itself has stopped answering.  Nothing is known about
 * any device now, so the whole aggregate image - health bytes included - falls
 * under the adapter-level policy.  Below the threshold this holds, for the
 * same reason as everywhere else: one missed frame must not inject an edge.
 *
 * Note that under CLEAR the health bytes zero too, which reads as
 * EIP_DEVICE_OFFLINE for every device.  That is the correct thing to tell a
 * POU when the scanner is gone.
 */
static plc_status_t proxy_fail(scanner_proxy_t *p, void *in, size_t in_len,
                               plc_status_t reason) {
    p->stats.timeouts++;
    p->stats.consecutive_timeouts++;

    if (p->stats.consecutive_timeouts >= p->caps.consecutive_timeout_threshold) {
        if (p->state != PLC_ADAPTER_FAULTED) {
            p->state = PLC_ADAPTER_FAULTED;
            p->stats.failsafe_activations++;
            PLC_LOG_WARN("eip-scanner '%s': %llu consecutive misses, "
                         "applying %s failsafe to all %u devices",
                         p->caps.name,
                         (unsigned long long)p->stats.consecutive_timeouts,
                         p->caps.failsafe_policy == PLC_FAILSAFE_CLEAR
                             ? "CLEAR" : "HOLD",
                         p->map ? p->map->device_count : 0);
        }
        plc_adapter_apply_failsafe(p->caps.failsafe_policy, in, p->last_good, in_len);
    } else {
        if (p->state == PLC_ADAPTER_ONLINE) p->state = PLC_ADAPTER_DEGRADED;
        plc_adapter_apply_failsafe(PLC_FAILSAFE_HOLD, in, p->last_good, in_len);
    }
    return reason;
}

static plc_status_t proxy_exchange(plc_protocol_adapter_t *self,
                                   const void *out, size_t out_len,
                                   void *in, size_t in_len) {
    scanner_proxy_t *p = self->impl;
    if (p->state == PLC_ADAPTER_CLOSED || !p->map) return PLC_ERR_STATE;
    if (out_len > p->caps.output_bytes || in_len > p->caps.input_bytes) {
        return PLC_ERR_INVAL;
    }

    const uint64_t t0  = now_us();
    const uint32_t seq = p->next_seq++;

    plc_spsc_drain(&p->map->rsp);
    plc_sem_drain(p->sem_rsp);

    plc_status_t st = plc_spsc_push(&p->map->req, seq, out, (uint32_t)out_len);
    if (st != PLC_OK) return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);
    sem_post(p->sem_req);

    const uint32_t budget = p->caps.exchange_timeout_us;
    int matched = 0;
    while (!matched) {
        const uint64_t spent = now_us() - t0;
        if (spent >= budget) return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);

        st = plc_sem_wait_timeout(p->sem_rsp, (uint32_t)(budget - spent));
        if (st == PLC_ERR_TIMEOUT) return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);
        if (st != PLC_OK) {
            p->stats.protocol_errors++;
            return proxy_fail(p, in, in_len, PLC_ERR_IO);
        }

        /* Drain the ring rather than assuming one post per queued frame: the
         * two are resynchronised separately above, so a reply landing between
         * those steps has no post left to wake us with. */
        for (;;) {
            st = plc_spsc_pop(&p->map->rsp, &p->scratch, PLC_IPC_MAX_FRAME_BYTES);
            if (st == PLC_ERR_AGAIN) break;
            if (st != PLC_OK) {
                p->stats.protocol_errors++;
                return proxy_fail(p, in, in_len, PLC_ERR_PROTO);
            }
            if ((int32_t)(p->scratch.seq - seq) < 0) continue;  /* late, drop */
            if (p->scratch.seq != seq) {
                p->stats.protocol_errors++;
                return proxy_fail(p, in, in_len, PLC_ERR_PROTO);
            }
            matched = 1;
            break;
        }
    }

    const size_t got = (p->scratch.len < in_len) ? p->scratch.len : in_len;
    if (in && in_len) {
        if (got) memcpy(in, p->scratch.data, got);
        if (got < in_len) memset((uint8_t *)in + got, 0, in_len - got);
        memcpy(p->last_good, in, in_len);
    }

    const uint64_t rtt = now_us() - t0;
    p->stats.exchanges++;
    p->stats.consecutive_timeouts = 0;
    p->stats.last_rtt_us = rtt;
    if (rtt > p->stats.max_rtt_us) p->stats.max_rtt_us = rtt;

    if (p->state != PLC_ADAPTER_ONLINE) {
        PLC_LOG_INFO("eip-scanner '%s': online", p->caps.name);
        p->state = PLC_ADAPTER_ONLINE;
    }
    return PLC_OK;
}

/* --- introspection ------------------------------------------------------- */

static plc_status_t proxy_caps(const plc_protocol_adapter_t *self,
                               plc_adapter_caps_t *out) {
    *out = ((const scanner_proxy_t *)self->impl)->caps;
    return PLC_OK;
}

static plc_status_t proxy_stats(const plc_protocol_adapter_t *self,
                                plc_adapter_stats_t *out) {
    *out = ((const scanner_proxy_t *)self->impl)->stats;
    return PLC_OK;
}

static plc_adapter_state_t proxy_state(const plc_protocol_adapter_t *self) {
    return ((const scanner_proxy_t *)self->impl)->state;
}

uint32_t plc_eip_scanner_devices_online(const plc_protocol_adapter_t *self);

/** Devices currently connected.  Status only - a POU reads the health bytes in
 *  the input image instead, which stay meaningful when the peer is gone. */
uint32_t plc_eip_scanner_devices_online(const plc_protocol_adapter_t *self) {
    if (!self || !self->impl) return 0;
    const scanner_proxy_t *p = self->impl;
    return p->map ? atomic_load(&p->map->status.devices_online) : 0;
}

static const plc_adapter_vtbl_t kScannerVtbl = {
    .open      = proxy_open,
    .close     = proxy_close,
    .exchange  = proxy_exchange,
    .get_caps  = proxy_caps,
    .get_stats = proxy_stats,
    .state     = proxy_state,
};

static plc_protocol_adapter_t *scanner_create(void) {
    plc_protocol_adapter_t *a = calloc(1, sizeof(*a));
    scanner_proxy_t *p = calloc(1, sizeof(*p));
    if (!a || !p) { free(a); free(p); return NULL; }
    p->state  = PLC_ADAPTER_CLOSED;
    p->shm.fd = -1;
    a->vtbl = &kScannerVtbl;
    a->impl = p;
    return a;
}

static void scanner_destroy(plc_protocol_adapter_t *a) {
    if (!a) return;
    if (a->impl) {
        proxy_close(a);
        free(a->impl);
    }
    free(a);
}

const plc_adapter_factory_t plc_eip_scanner_adapter_factory = {
    .protocol    = "ethernet-ip-scanner",
    .description = "EtherNet/IP Scanner/originator (EIPScanner) in a separate process",
    .create      = scanner_create,
    .destroy     = scanner_destroy,
};
