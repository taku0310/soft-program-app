/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/plc_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "softplc/plc_log.h"

typedef struct plc_program {
    const char *name;
    plc_pou_fn  fn;
    void       *user;
} plc_program_t;

struct plc_runtime {
    plc_runtime_config_t cfg;
    plc_process_image_t  pi;

    plc_io_binding_t bindings[PLC_MAX_BINDINGS];
    size_t           binding_count;

    plc_program_t programs[PLC_MAX_PROGRAMS];
    size_t        program_count;

    /* Outputs latched at the end of scan N, transmitted at the top of N+1.
     * Holding a shadow rather than reading %Q directly means a POU can keep
     * writing %Q while the I/O phase is in flight without tearing the frame. */
    uint8_t *q_shadow;
    size_t   q_shadow_len;

    volatile sig_atomic_t stop;
    uint64_t scan_no;
    PLC_TIME started_ns;
    PLC_TIME last_scan_ns;
    int      first_scan;

    plc_runtime_stats_t stats;
};

static PLC_TIME now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (PLC_TIME)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void plc_runtime_config_init(plc_runtime_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->cycle_us = 10000;   /* 10 ms: a common default for a soft PLC task */
    cfg->i_bytes  = 256;
    cfg->q_bytes  = 256;
    cfg->m_bytes  = 1024;
    cfg->continue_on_io_fault = 1;
}

plc_runtime_t *plc_runtime_create(const plc_runtime_config_t *cfg) {
    if (!cfg) return NULL;

    plc_runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    rt->cfg = *cfg;

    if (plc_pi_init(&rt->pi, cfg->i_bytes, cfg->q_bytes, cfg->m_bytes) != PLC_OK) {
        free(rt);
        return NULL;
    }

    rt->q_shadow_len = cfg->q_bytes;
    rt->q_shadow = calloc(rt->q_shadow_len ? rt->q_shadow_len : 1, 1);
    if (!rt->q_shadow) {
        plc_pi_free(&rt->pi);
        free(rt);
        return NULL;
    }

    rt->first_scan = 1;
    return rt;
}

void plc_runtime_destroy(plc_runtime_t *rt) {
    if (!rt) return;
    free(rt->q_shadow);
    plc_pi_free(&rt->pi);
    free(rt);
}

plc_process_image_t *plc_runtime_image(plc_runtime_t *rt) {
    return rt ? &rt->pi : NULL;
}

plc_status_t plc_runtime_bind_adapter(plc_runtime_t *rt,
                                      plc_protocol_adapter_t *adapter,
                                      size_t i_offset, size_t q_offset) {
    if (!rt || !adapter) return PLC_ERR_INVAL;
    if (rt->binding_count >= PLC_MAX_BINDINGS) return PLC_ERR_NOMEM;

    plc_adapter_caps_t caps;
    plc_status_t st = plc_adapter_get_caps(adapter, &caps);
    if (st != PLC_OK) return st;

    /* Reject an ABI mismatch here rather than mid-scan: a plugin built against
     * a different header is a configuration error, not a runtime condition. */
    if (caps.abi_version != PLC_ADAPTER_ABI_VERSION) {
        PLC_LOG_ERR("adapter '%s' ABI %u, core expects %u",
                    caps.name, caps.abi_version, PLC_ADAPTER_ABI_VERSION);
        return PLC_ERR_PROTO;
    }

    const size_t i_size = plc_pi_area_size(&rt->pi, PLC_AREA_I);
    const size_t q_size = plc_pi_area_size(&rt->pi, PLC_AREA_Q);
    if (i_offset > i_size || caps.input_bytes  > i_size - i_offset) return PLC_ERR_INVAL;
    if (q_offset > q_size || caps.output_bytes > q_size - q_offset) return PLC_ERR_INVAL;

    plc_io_binding_t *b = &rt->bindings[rt->binding_count++];
    b->adapter  = adapter;
    b->i_offset = i_offset;
    b->q_offset = q_offset;
    b->caps     = caps;

    PLC_LOG_INFO("bound %s adapter '%s': %%I[%zu..%zu) %%Q[%zu..%zu) failsafe=%s",
                 caps.protocol, caps.name,
                 i_offset, i_offset + caps.input_bytes,
                 q_offset, q_offset + caps.output_bytes,
                 caps.failsafe_policy == PLC_FAILSAFE_CLEAR ? "CLEAR" : "HOLD");
    return PLC_OK;
}

plc_status_t plc_runtime_add_program(plc_runtime_t *rt, const char *name,
                                     plc_pou_fn fn, void *user) {
    if (!rt || !fn) return PLC_ERR_INVAL;
    if (rt->program_count >= PLC_MAX_PROGRAMS) return PLC_ERR_NOMEM;
    plc_program_t *p = &rt->programs[rt->program_count++];
    p->name = name ? name : "<unnamed>";
    p->fn   = fn;
    p->user = user;
    return PLC_OK;
}

/* Phase 1: one exchange per adapter.  A timeout is not fatal - the adapter has
 * already written a failsafe image into %I - but it is counted and reported. */
static plc_status_t scan_io(plc_runtime_t *rt) {
    plc_status_t worst = PLC_OK;

    for (size_t i = 0; i < rt->binding_count; ++i) {
        plc_io_binding_t *b = &rt->bindings[i];
        uint8_t *i_base = plc_pi_area(&rt->pi, PLC_AREA_I);
        if (!i_base) return PLC_ERR_STATE;

        const void *out = (b->caps.output_bytes && b->q_offset <= rt->q_shadow_len)
                        ? rt->q_shadow + b->q_offset : NULL;

        plc_status_t st = plc_adapter_exchange(b->adapter,
                                               out, b->caps.output_bytes,
                                               i_base + b->i_offset,
                                               b->caps.input_bytes);
        if (st != PLC_OK) worst = st;
    }
    return worst;
}

plc_status_t plc_runtime_scan_once(plc_runtime_t *rt) {
    if (!rt) return PLC_ERR_INVAL;

    const PLC_TIME t0 = now_ns();
    if (rt->first_scan) {
        rt->started_ns   = t0;
        rt->last_scan_ns = t0;
    }

    /* --- 1. INPUT ------------------------------------------------------- */
    const plc_status_t io_st = scan_io(rt);
    if (io_st != PLC_OK) {
        rt->stats.io_faults++;
        if (!rt->cfg.continue_on_io_fault) {
            PLC_LOG_ERR("scan %llu aborted: I/O fault (%s)",
                        (unsigned long long)rt->scan_no, plc_strerror(io_st));
            return io_st;
        }
    }

    /* --- 2. EXECUTE ----------------------------------------------------- */
    plc_pou_ctx_t ctx = {
        .pi    = &rt->pi,
        .now   = t0,
        .dt    = rt->first_scan ? 0 : (t0 - rt->last_scan_ns),
        .scan  = rt->scan_no,
        .first = (PLC_BOOL)rt->first_scan,
        .user  = NULL,
    };
    for (size_t i = 0; i < rt->program_count; ++i) {
        ctx.user = rt->programs[i].user;
        rt->programs[i].fn(&ctx);
    }

    /* --- 3. OUTPUT ------------------------------------------------------ */
    const uint8_t *q = plc_pi_area(&rt->pi, PLC_AREA_Q);
    if (q && rt->q_shadow_len) memcpy(rt->q_shadow, q, rt->q_shadow_len);

    /* --- accounting ----------------------------------------------------- */
    const PLC_TIME t1 = now_ns();
    const uint64_t dur_us = (uint64_t)((t1 - t0) / 1000);
    rt->stats.scans++;
    rt->stats.last_scan_us = dur_us;
    if (dur_us > rt->stats.max_scan_us) rt->stats.max_scan_us = dur_us;

    if (!rt->first_scan && rt->cfg.cycle_us) {
        const int64_t period_us = (int64_t)((t0 - rt->last_scan_ns) / 1000);
        int64_t jitter = period_us - (int64_t)rt->cfg.cycle_us;
        if (jitter < 0) jitter = -jitter;
        rt->stats.last_jitter_us = (uint64_t)jitter;
        if ((uint64_t)jitter > rt->stats.max_jitter_us) {
            rt->stats.max_jitter_us = (uint64_t)jitter;
        }
        if (dur_us > rt->cfg.cycle_us) rt->stats.overruns++;
    }

    rt->last_scan_ns = t0;
    rt->first_scan   = 0;
    rt->scan_no++;
    return io_st;
}

/* Absolute-deadline pacing.  Adding the period to the previous deadline (not
 * to "now") keeps the average period exact and stops one long scan from
 * shifting every scan after it. */
static void sleep_until(PLC_TIME deadline_ns) {
    struct timespec ts = {
        .tv_sec  = (time_t)(deadline_ns / 1000000000),
        .tv_nsec = (long)(deadline_ns % 1000000000),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        /* retry: a signal must not shorten the cycle */
    }
}

plc_status_t plc_runtime_run(plc_runtime_t *rt, uint64_t max_scans) {
    if (!rt) return PLC_ERR_INVAL;
    rt->stop = 0;

    const PLC_TIME period = (PLC_TIME)rt->cfg.cycle_us * 1000;
    PLC_TIME deadline = now_ns();

    while (!rt->stop) {
        const plc_status_t st = plc_runtime_scan_once(rt);
        if (st != PLC_OK && !rt->cfg.continue_on_io_fault) return st;

        if (max_scans && rt->scan_no >= max_scans) break;

        if (period > 0) {
            deadline += period;
            const PLC_TIME t = now_ns();
            if (deadline < t) {
                /* Fell far enough behind that catching up would mean a burst
                 * of back-to-back scans.  Re-base instead: a soft PLC should
                 * lose cycles, not run them faster than real time. */
                deadline = t;
            }
            sleep_until(deadline);
        }
    }
    return PLC_OK;
}

void plc_runtime_request_stop(plc_runtime_t *rt) {
    if (rt) rt->stop = 1;
}

void plc_runtime_get_stats(const plc_runtime_t *rt, plc_runtime_stats_t *out) {
    if (!rt || !out) return;
    *out = rt->stats;
}

size_t plc_runtime_binding_count(const plc_runtime_t *rt) {
    return rt ? rt->binding_count : 0;
}

const plc_io_binding_t *plc_runtime_binding_at(const plc_runtime_t *rt, size_t i) {
    if (!rt || i >= rt->binding_count) return NULL;
    return &rt->bindings[i];
}
