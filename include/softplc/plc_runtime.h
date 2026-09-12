/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plc_runtime.h
 * @brief IEC 61131-3 cyclic scan engine.
 *
 * One configuration owns one process image, N I/O bindings and N programs.
 * A scan is the classic three phases:
 *
 *     1. INPUT   for each binding: adapter->exchange(shadow %Q, %I slice)
 *     2. EXECUTE for each program in configured order: pou(ctx)
 *     3. OUTPUT  %Q is latched into the shadow that phase 1 will transmit
 *
 * Outputs computed in scan N therefore reach the field at the top of scan N+1.
 * That is one deliberate scan of latency in exchange for a single, bounded
 * blocking point per cycle: adapters are asked for I/O exactly once, so the
 * worst case scan time is a sum of known budgets rather than a sum of two.
 */
#ifndef SOFTPLC_PLC_RUNTIME_H
#define SOFTPLC_PLC_RUNTIME_H

#include "softplc/process_image.h"
#include "softplc/protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MAX_BINDINGS 8
#define PLC_MAX_PROGRAMS 8

typedef struct plc_runtime plc_runtime_t;

/** Execution context handed to every POU. */
typedef struct plc_pou_ctx {
    plc_process_image_t *pi;      /**< the process image                     */
    PLC_TIME             now;     /**< monotonic timestamp of this scan      */
    PLC_TIME             dt;      /**< elapsed since the previous scan       */
    uint64_t             scan;    /**< scan counter, starts at 0             */
    PLC_BOOL             first;   /**< true on the first scan after start    */
    void                *user;    /**< as registered with the program        */
} plc_pou_ctx_t;

/** A Program Organization Unit. */
typedef void (*plc_pou_fn)(plc_pou_ctx_t *ctx);

/**
 * @brief Binds one adapter's I/O images into slices of %I and %Q.
 *
 * The core copies @c output_bytes from %Q at @c q_offset out, and copies the
 * returned @c input_bytes into %I at @c i_offset.  Sizes come from the
 * adapter's caps, so a mis-sized binding is rejected at configuration time.
 */
typedef struct plc_io_binding {
    plc_protocol_adapter_t *adapter;
    size_t                  i_offset;
    size_t                  q_offset;
    plc_adapter_caps_t      caps;      /**< snapshot taken at bind time */
} plc_io_binding_t;

/** Per-scan and cumulative timing, readable from outside the scan thread. */
typedef struct plc_runtime_stats {
    uint64_t scans;
    uint64_t overruns;         /**< scans that missed the cycle deadline   */
    uint64_t io_faults;        /**< scans where at least one adapter timed out */
    uint64_t last_scan_us;
    uint64_t max_scan_us;
    uint64_t last_jitter_us;   /**< |actual period - configured period|    */
    uint64_t max_jitter_us;
} plc_runtime_stats_t;

typedef struct plc_runtime_config {
    uint32_t cycle_us;   /**< task period; 0 = free running                */
    size_t   i_bytes;
    size_t   q_bytes;
    size_t   m_bytes;
    /** Keep scanning when an adapter is FAULTED.  A soft PLC that stops on a
     *  fieldbus fault is usually worse than one that runs on failsafe values,
     *  so this defaults to true; set false for a hard interlock. */
    int      continue_on_io_fault;
} plc_runtime_config_t;

void plc_runtime_config_init(plc_runtime_config_t *cfg);

plc_runtime_t *plc_runtime_create(const plc_runtime_config_t *cfg);
void           plc_runtime_destroy(plc_runtime_t *rt);

/** The image; valid for the runtime's lifetime. */
plc_process_image_t *plc_runtime_image(plc_runtime_t *rt);

/**
 * @brief Wire an opened adapter into the image.
 *
 * The adapter must already be open so that its caps are final.  Fails with
 * ::PLC_ERR_INVAL if the requested slices do not fit the image, and with
 * ::PLC_ERR_PROTO on an ABI mismatch.
 */
plc_status_t plc_runtime_bind_adapter(plc_runtime_t *rt,
                                      plc_protocol_adapter_t *adapter,
                                      size_t i_offset, size_t q_offset);

plc_status_t plc_runtime_add_program(plc_runtime_t *rt,
                                     const char *name,
                                     plc_pou_fn fn, void *user);

/** Run exactly one scan.  Exposed so tests can step the engine. */
plc_status_t plc_runtime_scan_once(plc_runtime_t *rt);

/**
 * @brief Run the cyclic task until plc_runtime_request_stop().
 *
 * Paces on CLOCK_MONOTONIC with absolute deadlines, so a long scan does not
 * push every later scan late.
 *
 * @param max_scans 0 = until stopped, otherwise stop after N scans.
 */
plc_status_t plc_runtime_run(plc_runtime_t *rt, uint64_t max_scans);

/** Async-signal-safe.  Safe to call from a signal handler. */
void plc_runtime_request_stop(plc_runtime_t *rt);

void plc_runtime_get_stats(const plc_runtime_t *rt, plc_runtime_stats_t *out);
size_t plc_runtime_binding_count(const plc_runtime_t *rt);
const plc_io_binding_t *plc_runtime_binding_at(const plc_runtime_t *rt, size_t i);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PLC_RUNTIME_H */
