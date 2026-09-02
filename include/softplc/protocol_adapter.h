/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file protocol_adapter.h
 * @brief Loosely-coupled protocol adapter interface (SIFB-inspired).
 *
 * The PLC core stays IEC 61131-3.  Fieldbus and OT protocol stacks are pulled
 * in behind a single interface whose shape is borrowed from the IEC 61499
 * Service Interface Function Block idea: a self-describing block with a
 * declared capability set, an explicit lifecycle, and exactly one cyclic
 * service call.  We import the *decoupling idea only* - no 61499 execution
 * model, no event-driven scheduling.  Scheduling stays 61131-3 cyclic.
 *
 * Design invariants every adapter must honour:
 *
 *  1. COPY SEMANTICS (memory model B).  An adapter never receives a pointer
 *     into the PLC process image and never hands out a pointer into its own
 *     buffers.  ::plc_adapter_vtbl::exchange copies out of the caller's
 *     output buffer and into the caller's input buffer before it returns.
 *     Where the adapter lives in another process the copy goes through an
 *     SPSC ring in shared memory; where it is in-process it is a memcpy.
 *
 *  2. BOUNDED TIME.  ::plc_adapter_vtbl::exchange is called from the scan and
 *     must return within ::plc_adapter_caps::exchange_timeout_us.  It is the
 *     only liveness signal the core has: there is no separate watchdog
 *     channel, because a peer that cannot answer within the budget is
 *     indistinguishable - to the core - from a peer that is dead.
 *
 *  3. FAILSAFE IS THE ADAPTER'S JOB.  On a timeout the adapter itself fills
 *     the caller's input buffer according to ::plc_adapter_caps::failsafe_policy
 *     and returns ::PLC_ERR_TIMEOUT.  The core never sees an uninitialised or
 *     torn input image, whatever happens to the peer.
 *
 *  4. CRASH CONTAINMENT ONLY.  Out-of-process adapters exist so that a stack
 *     fault cannot corrupt PLC memory.  Restarting a dead adapter process is
 *     deliberately NOT handled here; it belongs to the supervisor or the
 *     container orchestrator.  The core degrades to failsafe and keeps
 *     scanning.
 */
#ifndef SOFTPLC_PROTOCOL_ADAPTER_H
#define SOFTPLC_PROTOCOL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bumped on any incompatible change to the structures below. */
#define PLC_ADAPTER_ABI_VERSION 1u

#define PLC_ADAPTER_NAME_MAX     32
/* Wide enough for the longest protocol identifier in use plus room: 16 could
 * not hold "ethernet-ip-scanner", and truncating a protocol name would make
 * two adapters indistinguishable in the registry. Widened while ABI version 1
 * is still unreleased, so no bump is warranted - there is no compiled adapter
 * anywhere that was built against the narrower field. */
#define PLC_ADAPTER_PROTOCOL_MAX 32

/**
 * @brief What the core does with the input image when the peer stops answering.
 *
 * Selectable per adapter through ::plc_adapter_config::failsafe_policy and
 * reported back through ::plc_adapter_caps::failsafe_policy.  There is no
 * global default: "hold" and "clear" are both wrong for some plants, so the
 * choice is always explicit.
 */
typedef enum plc_failsafe_policy {
    PLC_FAILSAFE_HOLD  = 0, /**< keep the last successfully received image */
    PLC_FAILSAFE_CLEAR = 1  /**< zero the input image                      */
} plc_failsafe_policy_t;

/** Lifecycle / connectivity state as reported by ::plc_adapter_vtbl::state. */
typedef enum plc_adapter_state {
    PLC_ADAPTER_CLOSED = 0, /**< before open() / after close()                */
    PLC_ADAPTER_OPENING,    /**< transport up, peer not exchanging yet        */
    PLC_ADAPTER_ONLINE,     /**< exchanging normally                          */
    PLC_ADAPTER_DEGRADED,   /**< timeouts seen, below the failsafe threshold  */
    PLC_ADAPTER_FAULTED     /**< threshold crossed, failsafe image applied    */
} plc_adapter_state_t;

/** Optional capability bits reported in ::plc_adapter_caps::flags. */
#define PLC_ADAPTER_CAP_OUT_OF_PROCESS  (1u << 0) /**< runs behind an IPC boundary */
#define PLC_ADAPTER_CAP_POINT_OVERRIDES (1u << 1) /**< reserved, see below         */

/**
 * @brief Per-point override descriptor.
 *
 * RESERVED.  The type is fixed now so that the ABI does not have to change
 * when forcing/simulation lands, but no adapter implements it yet: every
 * adapter leaves ::plc_adapter_caps::point_override_capacity at 0 and the core
 * never populates a table.  Do not add behaviour behind this without bumping
 * ::PLC_ADAPTER_ABI_VERSION.
 */
typedef struct plc_point_override {
    uint16_t point_index; /**< index into the adapter's input image, in bits */
    uint8_t  mode;        /**< reserved; 0 = inactive                       */
    uint8_t  reserved;    /**< reserved; must be 0                          */
    uint32_t value;       /**< reserved; forced value, LSB-aligned          */
} plc_point_override_t;

/**
 * @brief Self-description of an adapter instance.
 *
 * Filled by the adapter, read by the core after open().  The core validates
 * @c abi_version and the image sizes before it wires the adapter into the I/O
 * map, so a mismatched plugin fails at configuration time rather than mid-scan.
 */
typedef struct plc_adapter_caps {
    uint32_t abi_version;                       /**< must equal ::PLC_ADAPTER_ABI_VERSION */
    char     name[PLC_ADAPTER_NAME_MAX];        /**< instance name, e.g. "eip0"           */
    char     protocol[PLC_ADAPTER_PROTOCOL_MAX];/**< family, e.g. "ethernet-ip"           */

    uint32_t input_bytes;   /**< field -> PLC, size of the image exchange() fills */
    uint32_t output_bytes;  /**< PLC -> field, size exchange() consumes           */

    plc_failsafe_policy_t failsafe_policy;
    uint32_t exchange_timeout_us;  /**< per-call budget; 0 = non-blocking */
    /** How long without a fresh image before the failsafe policy is applied.
     *  A duration rather than a count of missed exchanges - see the field of
     *  the same name in ::plc_adapter_config for why. */
    uint32_t failsafe_timeout_us;

    uint32_t flags;                    /**< PLC_ADAPTER_CAP_* bits                 */
    uint32_t point_override_capacity;  /**< RESERVED, always 0 in this release     */
} plc_adapter_caps_t;

/**
 * @brief Configuration handed to ::plc_adapter_vtbl::open.
 *
 * Everything an adapter needs that is not protocol-private.  Protocol-private
 * settings travel in @c endpoint, which each adapter parses itself.
 */
typedef struct plc_adapter_config {
    const char *name;      /**< instance name; also namespaces IPC objects */
    const char *endpoint;  /**< adapter-specific, e.g. "eth0" or "0.0.0.0:44818" */

    uint32_t input_bytes;  /**< requested image sizes; 0 = adapter's own default */
    uint32_t output_bytes;

    plc_failsafe_policy_t failsafe_policy;
    uint32_t exchange_timeout_us;  /**< 0 = adapter default */

    /**
     * @brief How long without a fresh image before failsafe.  0 = default.
     *
     * A **duration**, deliberately, not a count of consecutive misses.
     *
     * This was a count, and measurement showed why that was the wrong shape.
     * Timeouts do not arrive independently: a host stall withholds data for
     * tens of milliseconds and takes out every scan inside it, so the run
     * length is really `stall / cycle`. Tuning a count against an observed
     * maximum just gets beaten by the next longer stall - which is exactly
     * what happened here, twice.
     *
     * Worse, a count silently changes meaning when the task period changes:
     * the same "3" tolerates 30 ms at a 10 ms cycle and 3 ms at 1 ms, so
     * retuning the scan rate would quietly retune the failsafe behaviour with
     * it. A duration is the quantity anyone actually reasons about, and it is
     * what CIP itself uses (a multiplier on RPI, i.e. a time).
     *
     * See ADR 0009 for the measurement.
     */
    uint32_t failsafe_timeout_us;
} plc_adapter_config_t;

/** Zero-initialised config with the neutral defaults. */
void plc_adapter_config_init(plc_adapter_config_t *cfg);

/** Cumulative counters, cleared by open(). */
typedef struct plc_adapter_stats {
    uint64_t exchanges;            /**< successful exchange() calls          */
    uint64_t timeouts;             /**< exchange() calls that timed out      */
    /** Microseconds since the last fresh image; 0 while online.  This is what
     *  the failsafe decision is made on, so it is the number to watch. */
    uint64_t stale_for_us;
    uint64_t failsafe_activations; /**< transitions into PLC_ADAPTER_FAULTED */
    uint64_t protocol_errors;      /**< framing / sequence faults            */
    uint64_t last_rtt_us;          /**< round trip of the last exchange()    */
    uint64_t max_rtt_us;           /**< worst round trip since open()        */
} plc_adapter_stats_t;

typedef struct plc_protocol_adapter plc_protocol_adapter_t;

/**
 * @brief The adapter interface proper.
 *
 * All entry points are called from the scan thread and none of them may block
 * indefinitely.  @c open and @c close may block for as long as the
 * configuration phase allows; @c exchange may not.
 */
typedef struct plc_adapter_vtbl {
    /** Acquire transport resources.  CLOSED -> OPENING. */
    plc_status_t (*open)(plc_protocol_adapter_t *self,
                         const plc_adapter_config_t *cfg);

    /** Release everything acquired by open().  Idempotent. */
    plc_status_t (*close)(plc_protocol_adapter_t *self);

    /**
     * @brief One cyclic I/O exchange.  Called exactly once per scan.
     *
     * Copies @p out_len bytes of PLC output image to the field and fills
     * @p in with @p in_len bytes of field input image.
     *
     * @return ::PLC_OK on a fresh exchange.  ::PLC_ERR_TIMEOUT when the peer
     *         missed its budget - @p in is still fully written, holding the
     *         last good image until ::plc_adapter_caps::failsafe_timeout_us
     *         has elapsed and then per the configured policy, so the caller
     *         may use it either way.
     */
    plc_status_t (*exchange)(plc_protocol_adapter_t *self,
                             const void *out, size_t out_len,
                             void *in, size_t in_len);

    plc_status_t (*get_caps)(const plc_protocol_adapter_t *self,
                             plc_adapter_caps_t *out);
    plc_status_t (*get_stats)(const plc_protocol_adapter_t *self,
                              plc_adapter_stats_t *out);
    plc_adapter_state_t (*state)(const plc_protocol_adapter_t *self);
} plc_adapter_vtbl_t;

/** Handle the core holds.  @c impl is private to the implementation. */
struct plc_protocol_adapter {
    const plc_adapter_vtbl_t *vtbl;
    void                     *impl;
};

/* --- thin call wrappers (argument checking lives here, not in every impl) - */

plc_status_t plc_adapter_open(plc_protocol_adapter_t *a,
                              const plc_adapter_config_t *cfg);
plc_status_t plc_adapter_close(plc_protocol_adapter_t *a);
plc_status_t plc_adapter_exchange(plc_protocol_adapter_t *a,
                                  const void *out, size_t out_len,
                                  void *in, size_t in_len);
plc_status_t plc_adapter_get_caps(const plc_protocol_adapter_t *a,
                                  plc_adapter_caps_t *out);
plc_status_t plc_adapter_get_stats(const plc_protocol_adapter_t *a,
                                   plc_adapter_stats_t *out);
plc_adapter_state_t plc_adapter_state(const plc_protocol_adapter_t *a);

/** Human readable state name.  Never NULL. */
const char *plc_adapter_state_name(plc_adapter_state_t s);

/**
 * @brief Tracks how long an adapter has gone without a fresh image.
 *
 * Shared so that the failsafe rule has exactly one implementation: three
 * adapters each counting their own way is how HOLD and CLEAR end up meaning
 * subtly different things per protocol.
 */
typedef struct plc_staleness {
    uint64_t last_good_us;   /**< monotonic timestamp of the last fresh image */
    int      have_good;      /**< false until the first successful exchange   */
} plc_staleness_t;

/** Monotonic microseconds.  Exposed because adapters need the same clock the
 *  staleness helper uses. */
uint64_t plc_now_us(void);

/** Record a fresh image. */
void plc_staleness_mark_fresh(plc_staleness_t *st);

/**
 * @brief How long since the last fresh image, in microseconds.
 *
 * Before the first successful exchange there is no "last good" to measure
 * from, so this reports the time since @p opened_us instead - otherwise an
 * adapter whose peer never appears would look perpetually fresh and never
 * reach failsafe.
 */
uint64_t plc_staleness_age_us(const plc_staleness_t *st, uint64_t opened_us);

/**
 * @brief Apply @p policy to @p in.
 *
 * Shared by every adapter so that HOLD and CLEAR mean exactly the same thing
 * regardless of protocol.  @p last_good must hold @p len bytes.
 */
void plc_adapter_apply_failsafe(plc_failsafe_policy_t policy,
                                void *in, const void *last_good, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PROTOCOL_ADAPTER_H */
