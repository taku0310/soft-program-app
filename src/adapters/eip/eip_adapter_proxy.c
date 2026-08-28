/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_adapter_proxy.c
 * @brief PLC-core side of the EtherNet/IP adapter: a ProtocolAdapter over IPC.
 *
 * From the scan engine's point of view this is an ordinary adapter.  All it
 * actually does is marshal one frame each way through the shared-memory rings
 * and enforce the two guarantees the core depends on:
 *
 *   - exchange() returns within the configured budget, always;
 *   - the input image is fully written before it returns, whatever the peer
 *     did or failed to do.
 *
 * There is no watchdog thread and no liveness ping.  A peer that is alive
 * answers within the budget, and one that is dead, wedged, or paused by the
 * scheduler is - to a control loop - the same condition.  Adding a second
 * channel to distinguish them would only create a way for the two signals to
 * disagree.  Nor does this file restart anything: crash containment is the
 * goal, and recovery is the supervisor's job.
 *
 * Reply matching: every request carries a sequence number the adapter echoes.
 * After a timeout a late reply may still be sitting in the ring, so the next
 * exchange discards everything whose seq is older than the one it is waiting
 * for.  Without this, one slow cycle would leave the core permanently one
 * frame behind, reading each scan's inputs on the following scan.
 */
#include "eip_shm_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "softplc/adapter_registry.h"
#include "softplc/ipc/shm.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

typedef struct eip_proxy {
    plc_adapter_caps_t  caps;
    plc_adapter_stats_t stats;
    plc_adapter_state_t state;

    char      instance[EIP_INSTANCE_MAX];
    plc_shm_t shm;
    eip_shm_t *map;
    sem_t    *sem_req;
    sem_t    *sem_rsp;
    char      sem_req_name[EIP_SHM_NAME_MAX];
    char      sem_rsp_name[EIP_SHM_NAME_MAX];

    uint32_t next_seq;
    uint8_t  last_good[PLC_IPC_MAX_FRAME_BYTES];

    plc_ipc_frame_t scratch;   /* pre-allocated: exchange() must not malloc */
} eip_proxy_t;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t env_u32(const char *key, uint32_t fallback) {
    const char *v = getenv(key);
    if (!v || !*v) return fallback;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || n == 0 || n > 0xFFFFFFFFul) return fallback;
    return (uint32_t)n;
}

/* --- lifecycle ----------------------------------------------------------- */

static void proxy_teardown(eip_proxy_t *p) {
    /* The core owns every IPC object, so it is the one that unlinks them.
     * Doing it here rather than in the adapter is what lets the adapter die
     * at any moment without leaving the namespace poisoned. */
    plc_sem_close(p->sem_req, p->sem_req_name, 1);
    plc_sem_close(p->sem_rsp, p->sem_rsp_name, 1);
    p->sem_req = p->sem_rsp = NULL;
    plc_shm_close(&p->shm);
    p->map = NULL;
}

static plc_status_t proxy_open(plc_protocol_adapter_t *self,
                               const plc_adapter_config_t *cfg) {
    eip_proxy_t *p = self->impl;
    if (p->state != PLC_ADAPTER_CLOSED) return PLC_ERR_STATE;

    snprintf(p->instance, sizeof(p->instance), "%s",
             (cfg->endpoint && *cfg->endpoint) ? cfg->endpoint
             : (cfg->name && *cfg->name) ? cfg->name : "default");

    const uint32_t in  = cfg->input_bytes
        ? cfg->input_bytes
        : env_u32("SOFTPLC_EIP_INPUT_BYTES", EIP_DEFAULT_INPUT_BYTES);
    const uint32_t out = cfg->output_bytes
        ? cfg->output_bytes
        : env_u32("SOFTPLC_EIP_OUTPUT_BYTES", EIP_DEFAULT_OUTPUT_BYTES);
    if (in > PLC_IPC_MAX_FRAME_BYTES || out > PLC_IPC_MAX_FRAME_BYTES) {
        PLC_LOG_ERR("eip: image %u/%u exceeds frame limit %u",
                    in, out, (unsigned)PLC_IPC_MAX_FRAME_BYTES);
        return PLC_ERR_INVAL;
    }

    char shm_name[EIP_SHM_NAME_MAX];
    eip_shm_name(shm_name, sizeof(shm_name), p->instance);
    eip_sem_req_name(p->sem_req_name, sizeof(p->sem_req_name), p->instance);
    eip_sem_rsp_name(p->sem_rsp_name, sizeof(p->sem_rsp_name), p->instance);

    plc_status_t st = plc_shm_create(&p->shm, shm_name, sizeof(eip_shm_t));
    if (st != PLC_OK) return st;
    p->map = p->shm.base;

    /* Publish the layout before the doorbells exist, so the adapter cannot
     * observe a half-initialised region: it can only attach to the semaphores
     * after this is all in place. */
    p->map->abi_version       = EIP_SHM_ABI_VERSION;
    p->map->layout_bytes      = (uint32_t)sizeof(eip_shm_t);
    p->map->output_bytes      = out;
    p->map->input_bytes       = in;
    p->map->produced_assembly = env_u32("SOFTPLC_EIP_PRODUCED_ASSEMBLY",
                                        EIP_DEFAULT_PRODUCED_ASSEMBLY);
    p->map->consumed_assembly = env_u32("SOFTPLC_EIP_CONSUMED_ASSEMBLY",
                                        EIP_DEFAULT_CONSUMED_ASSEMBLY);
    plc_spsc_init(&p->map->req);
    plc_spsc_init(&p->map->rsp);
    atomic_store(&p->map->status.adapter_state, PLC_ADAPTER_CLOSED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->map->magic = EIP_SHM_MAGIC;   /* last: this is the attach gate */

    if ((st = plc_sem_create(&p->sem_req, p->sem_req_name)) != PLC_OK ||
        (st = plc_sem_create(&p->sem_rsp, p->sem_rsp_name)) != PLC_OK) {
        proxy_teardown(p);
        return st;
    }

    plc_adapter_caps_t *c = &p->caps;
    memset(c, 0, sizeof(*c));
    c->abi_version = PLC_ADAPTER_ABI_VERSION;
    snprintf(c->name, sizeof(c->name), "%s", cfg->name ? cfg->name : "eip0");
    snprintf(c->protocol, sizeof(c->protocol), "ethernet-ip");
    c->input_bytes  = in;
    c->output_bytes = out;
    c->failsafe_policy = cfg->failsafe_policy;
    c->exchange_timeout_us = cfg->exchange_timeout_us
        ? cfg->exchange_timeout_us
        : env_u32("SOFTPLC_EIP_EXCHANGE_TIMEOUT_US", EIP_DEFAULT_EXCHANGE_TIMEOUT_US);
    c->consecutive_timeout_threshold = cfg->consecutive_timeout_threshold
        ? cfg->consecutive_timeout_threshold
        : env_u32("SOFTPLC_EIP_TIMEOUT_THRESHOLD", EIP_DEFAULT_TIMEOUT_THRESHOLD);
    c->flags = PLC_ADAPTER_CAP_OUT_OF_PROCESS;
    c->point_override_capacity = 0;   /* reserved; see protocol_adapter.h */

    memset(&p->stats, 0, sizeof(p->stats));
    memset(p->last_good, 0, sizeof(p->last_good));
    p->next_seq = 1;

    /* OPENING, not ONLINE: the adapter process may not even have started.
     * The first successful exchange is what promotes this. */
    p->state = PLC_ADAPTER_OPENING;

    PLC_LOG_INFO("eip proxy '%s' ready on %s (in=%u out=%u timeout=%uus threshold=%u failsafe=%s)",
                 c->name, shm_name, in, out,
                 c->exchange_timeout_us, c->consecutive_timeout_threshold,
                 c->failsafe_policy == PLC_FAILSAFE_CLEAR ? "CLEAR" : "HOLD");
    return PLC_OK;
}

static plc_status_t proxy_close(plc_protocol_adapter_t *self) {
    eip_proxy_t *p = self->impl;
    if (p->state == PLC_ADAPTER_CLOSED) return PLC_OK;
    proxy_teardown(p);
    p->state = PLC_ADAPTER_CLOSED;
    return PLC_OK;
}

/* --- the cyclic exchange -------------------------------------------------- */

/**
 * Record a missed exchange and write the input image the core will run on.
 *
 * Below the threshold we always HOLD, even when the configured policy is
 * CLEAR: a single dropped frame is a transport hiccup, and zeroing the image
 * for one scan would inject a falling edge on every input that a POU would
 * see as a real event.  CLEAR is about a peer that is gone, and that is what
 * the threshold decides.
 */
static plc_status_t proxy_fail(eip_proxy_t *p, void *in, size_t in_len,
                               plc_status_t reason) {
    p->stats.timeouts++;
    p->stats.consecutive_timeouts++;

    if (p->stats.consecutive_timeouts >= p->caps.consecutive_timeout_threshold) {
        if (p->state != PLC_ADAPTER_FAULTED) {
            p->state = PLC_ADAPTER_FAULTED;
            p->stats.failsafe_activations++;
            PLC_LOG_WARN("eip '%s': %llu consecutive misses, applying %s failsafe",
                         p->caps.name,
                         (unsigned long long)p->stats.consecutive_timeouts,
                         p->caps.failsafe_policy == PLC_FAILSAFE_CLEAR
                             ? "CLEAR" : "HOLD");
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
    eip_proxy_t *p = self->impl;
    if (p->state == PLC_ADAPTER_CLOSED || !p->map) return PLC_ERR_STATE;
    if (out_len > p->caps.output_bytes || in_len > p->caps.input_bytes) {
        return PLC_ERR_INVAL;
    }

    const uint64_t t0  = now_us();
    const uint32_t seq = p->next_seq++;

    /* Anything still queued predates this request and is stale by definition:
     * on a cyclic bus an old frame is worse than no frame. */
    plc_spsc_drain(&p->map->rsp);
    plc_sem_drain(p->sem_rsp);

    plc_status_t st = plc_spsc_push(&p->map->req, seq, out, (uint32_t)out_len);
    if (st != PLC_OK) {
        /* Full means the adapter has stopped consuming - it is wedged or gone.
         * Treat it as a miss rather than an error, so the same failsafe
         * escalation applies as for a silent peer. */
        return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);
    }
    sem_post(p->sem_req);

    /* Wait for *our* reply.  A reply that arrives while we hold a stale seq
     * is discarded and the wait continues on the remaining budget. */
    const uint32_t budget = p->caps.exchange_timeout_us;
    for (;;) {
        const uint64_t spent = now_us() - t0;
        if (spent >= budget) return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);

        st = plc_sem_wait_timeout(p->sem_rsp, (uint32_t)(budget - spent));
        if (st == PLC_ERR_TIMEOUT) return proxy_fail(p, in, in_len, PLC_ERR_TIMEOUT);
        if (st != PLC_OK) {
            p->stats.protocol_errors++;
            return proxy_fail(p, in, in_len, PLC_ERR_IO);
        }

        st = plc_spsc_pop(&p->map->rsp, &p->scratch, PLC_IPC_MAX_FRAME_BYTES);
        if (st == PLC_ERR_AGAIN) continue;          /* spurious post */
        if (st != PLC_OK) {
            p->stats.protocol_errors++;
            return proxy_fail(p, in, in_len, PLC_ERR_PROTO);
        }
        /* Signed comparison over the modular counter, so this stays correct
         * across the uint32_t wrap. */
        if ((int32_t)(p->scratch.seq - seq) < 0) continue;   /* late, drop it */
        if (p->scratch.seq != seq) {
            p->stats.protocol_errors++;
            return proxy_fail(p, in, in_len, PLC_ERR_PROTO);
        }
        break;
    }

    /* A short reply is not an error: the adapter may have fewer bytes of
     * consumed assembly configured than the binding asked for.  Zero-extend
     * so the image is always fully defined. */
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
        PLC_LOG_INFO("eip '%s': online", p->caps.name);
        p->state = PLC_ADAPTER_ONLINE;
    }
    return PLC_OK;
}

/* --- introspection ------------------------------------------------------- */

static plc_status_t proxy_caps(const plc_protocol_adapter_t *self,
                               plc_adapter_caps_t *out) {
    *out = ((const eip_proxy_t *)self->impl)->caps;
    return PLC_OK;
}

static plc_status_t proxy_stats(const plc_protocol_adapter_t *self,
                                plc_adapter_stats_t *out) {
    *out = ((const eip_proxy_t *)self->impl)->stats;
    return PLC_OK;
}

static plc_adapter_state_t proxy_state(const plc_protocol_adapter_t *self) {
    return ((const eip_proxy_t *)self->impl)->state;
}

uint32_t plc_eip_io_connections(const plc_protocol_adapter_t *self);

/** Established CIP I/O connections, for status reporting only. */
uint32_t plc_eip_io_connections(const plc_protocol_adapter_t *self) {
    if (!self || !self->impl) return 0;
    const eip_proxy_t *p = self->impl;
    if (!p->map) return 0;
    return atomic_load(&p->map->status.io_connections);
}

static const plc_adapter_vtbl_t kEipVtbl = {
    .open      = proxy_open,
    .close     = proxy_close,
    .exchange  = proxy_exchange,
    .get_caps  = proxy_caps,
    .get_stats = proxy_stats,
    .state     = proxy_state,
};

static plc_protocol_adapter_t *eip_create(void) {
    plc_protocol_adapter_t *a = calloc(1, sizeof(*a));
    eip_proxy_t *p = calloc(1, sizeof(*p));
    if (!a || !p) { free(a); free(p); return NULL; }
    p->state  = PLC_ADAPTER_CLOSED;
    p->shm.fd = -1;
    a->vtbl = &kEipVtbl;
    a->impl = p;
    return a;
}

static void eip_destroy(plc_protocol_adapter_t *a) {
    if (!a) return;
    if (a->impl) {
        proxy_close(a);
        free(a->impl);
    }
    free(a);
}

const plc_adapter_factory_t plc_eip_adapter_factory = {
    .protocol    = "ethernet-ip",
    .description = "EtherNet/IP adapter (OpENer) in a separate process",
    .create      = eip_create,
    .destroy     = eip_destroy,
};
