/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_shm_layout.h
 * @brief The contract between the PLC core and the EtherNet/IP adapter process.
 *
 * Both sides compile this header.  It is the only thing they share: there is
 * no shared code, no shared allocator and, critically, no shared PLC memory -
 * just this one mapped region with two SPSC rings in it.
 *
 * Object naming is namespaced by an instance string so that several soft PLCs
 * can run on one host (or in one Kubernetes pod) without colliding:
 *
 *     /softplc.<instance>.eip        POSIX shared memory
 *     /softplc.<instance>.eip.req    semaphore: core -> adapter doorbell
 *     /softplc.<instance>.eip.rsp    semaphore: adapter -> core doorbell
 *
 * Because POSIX shared memory and named semaphores both live in /dev/shm, two
 * containers share them by sharing that mount - see docker/docker-compose.yml.
 *
 * Ownership: the PLC core creates and unlinks the objects; the adapter only
 * ever opens them.  The core is the one that must survive an adapter crash, so
 * it owns the lifetime.
 *
 * Direction convention, fixed here once so neither side has to guess.  OpENer
 * implements an EtherNet/IP *adapter* (a CIP target), so an external scanner
 * owns the connection:
 *
 *     req ring: PLC %Q  -> adapter -> produced assembly  (T->O on the wire)
 *     rsp ring: PLC %I <-  adapter <- consumed assembly  (O->T on the wire)
 */
#ifndef SOFTPLC_EIP_SHM_LAYOUT_H
#define SOFTPLC_EIP_SHM_LAYOUT_H

#include <stdatomic.h>
#include <stdint.h>

#include "softplc/ipc/spsc_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/** "EIP1" - checked on attach so a stale region from another build is
 *  rejected rather than misinterpreted. */
#define EIP_SHM_MAGIC        0x45495031u
#define EIP_SHM_ABI_VERSION  1u

#define EIP_SHM_NAME_MAX     128
#define EIP_INSTANCE_MAX      32

/** Default EtherNet/IP assembly instance numbers.  Overridable via env so a
 *  device profile can be matched without a rebuild. */
#define EIP_DEFAULT_PRODUCED_ASSEMBLY  100  /**< 0x64, T->O, PLC outputs */
#define EIP_DEFAULT_CONSUMED_ASSEMBLY  150  /**< 0x96, O->T, PLC inputs  */
#define EIP_DEFAULT_CONFIG_ASSEMBLY    151  /**< 0x97                    */
#define EIP_DEFAULT_HEARTBEAT_INPUT_ONLY_ASSEMBLY  152
#define EIP_DEFAULT_HEARTBEAT_LISTEN_ONLY_ASSEMBLY 153

/** Default cyclic image sizes, in bytes. */
#define EIP_DEFAULT_INPUT_BYTES   32
#define EIP_DEFAULT_OUTPUT_BYTES  32

/**
 * @brief How long without a fresh image before the failsafe policy applies.
 *
 * A duration, not a count of missed exchanges. Five samples of 20 000
 * exchanges each (tools/bench_exchange.c, 10 ms period, 5 ms budget) on this
 * class of host:
 *
 *     longest consecutive run of timeouts: 3, 5, 2, 3, 2
 *     worst single round trip:  23.3, 26.8, 29.4, 33.4, 11.6 ms
 *     timeouts: 2 to 16 per 20 000 (0.01% to 0.08%)
 *
 * A run of five at a 10 ms period is ~50 ms with no fresh data, from host
 * scheduling alone, with a perfectly healthy peer. Two successive count-based
 * thresholds were set from an observed maximum and both were beaten by the
 * next sample - which is the point: timeouts are not independent. A stall
 * withholds data for tens of milliseconds and takes out every scan inside it,
 * so the run length is really stall/cycle and chasing the observed maximum
 * never converges.
 *
 * 100 ms is twice the worst disruption seen across 100 000 exchanges. Being a
 * duration it also stops meaning something different when the task period
 * changes, which a count silently did.
 *
 * RE-MEASURE ON THE TARGET HARDWARE. This describes a shared cloud host; a
 * PREEMPT_RT kernel would show a far shorter tail and justify a much lower
 * value, which is worth having because this is detection latency for a dead
 * peer. SOFTPLC_EIP_FAILSAFE_TIMEOUT_US overrides it without a rebuild.
 */
#define EIP_DEFAULT_FAILSAFE_TIMEOUT_US  100000u

/**
 * @brief Default per-exchange budget.
 *
 * Half the default 10 ms task period, so a timing-out adapter still leaves
 * the scan room to run its logic.  The measurement supports keeping it there:
 * p99.99 came in at 4534 us, just inside 5 ms, so this sits right at the knee
 * of the distribution.  Widening it to swallow the 23 ms outliers would mean
 * a timing-out adapter consuming most of the cycle, which is a worse trade
 * than absorbing those outliers as held frames.
 *
 * A non-zero timeout count on a healthy system is therefore expected, not a
 * fault: 0.075% of scans in the Phase 0 run, every one of them absorbed by
 * holding the last good image.
 */
#define EIP_DEFAULT_EXCHANGE_TIMEOUT_US  5000u

/** Adapter-side liveness/telemetry, published for observability only.
 *  The core never makes a control decision from these: a dead process cannot
 *  update them, which is exactly why exchange() timeout is the real check. */
typedef struct eip_adapter_status {
    _Atomic uint32_t adapter_state;    /**< plc_adapter_state_t              */
    _Atomic uint32_t io_connections;   /**< established CIP I/O connections  */
    _Atomic uint64_t cycles;           /**< adapter service loop iterations  */
    _Atomic uint64_t assembly_writes;  /**< consumed-assembly updates seen   */
    _Atomic uint64_t last_error;       /**< last EipStatus / errno observed  */
} eip_adapter_status_t;

/**
 * @brief The mapped region.
 *
 * Fixed size and pointer-free, so the two processes may map it at different
 * addresses.  Sizes are negotiated by the core at create time and merely read
 * by the adapter, so there is one authority for the layout.
 */
typedef struct eip_shm {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t layout_bytes;   /**< sizeof(eip_shm), cross-checked on attach */
    uint32_t output_bytes;   /**< PLC -> field image size */
    uint32_t input_bytes;    /**< field -> PLC image size */
    uint32_t produced_assembly;
    uint32_t consumed_assembly;
    uint32_t reserved[9];

    eip_adapter_status_t status;

    plc_spsc_ring_t req;  /**< core -> adapter */
    plc_spsc_ring_t rsp;  /**< adapter -> core */
} eip_shm_t;

/** Build "/softplc.<instance>.eip" and the two semaphore names into @p buf. */
void eip_shm_name(char *buf, size_t buf_len, const char *instance);
void eip_sem_req_name(char *buf, size_t buf_len, const char *instance);
void eip_sem_rsp_name(char *buf, size_t buf_len, const char *instance);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_EIP_SHM_LAYOUT_H */
