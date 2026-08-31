/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_shm_layout.h
 * @brief Contract between the PLC core and the EtherNet/IP Scanner process.
 *
 * Same shape as eip_shm_layout.h - two SPSC rings, two doorbells, the core
 * owns the objects - but for the *originator* role, and the difference is the
 * part most likely to cause a bug.
 *
 * ## The CIP direction labels invert
 *
 * `%I` and `%Q` do not change meaning; the wire-direction names do, because in
 * one role we are the target and in the other the originator:
 *
 *     Adapter role (eip_shm_layout.h)   PLC %Q -> produced assembly (T->O)
 *                                       PLC %I <- consumed assembly (O->T)
 *
 *     Scanner role (this file)          PLC %Q -> O->T   (we originate)
 *                                       PLC %I <- T->O
 *
 * Read that twice before editing either file. Anyone reasoning from the
 * adapter's comments while working here will get it exactly backwards, and the
 * result - outputs appearing on inputs - is the kind of fault that reaches
 * machinery before it reaches a log.
 *
 * ## Aggregate images
 *
 * One scanner process drives N remote devices, and the core sees one adapter
 * with one image pair: the devices' slices are concatenated in device-table
 * order. That keeps the scan to a single bounded `exchange()` however many
 * devices there are, instead of N sequential timeouts.
 *
 * ## Per-device health is data, not adapter state
 *
 * `plc_adapter_state_t` is one value, and "device 7 is down while the rest are
 * fine" is precisely the condition that matters. Rather than widen the adapter
 * interface, the scanner applies the failsafe policy *per device* to that
 * device's slice, and publishes a per-device health byte that the core copies
 * into the input image ahead of the I/O data. A POU reads it like any other
 * input. That is the right home for it: what to do when one drive drops is
 * plant logic, not runtime policy.
 */
#ifndef SOFTPLC_EIP_SCANNER_SHM_LAYOUT_H
#define SOFTPLC_EIP_SCANNER_SHM_LAYOUT_H

#include <stdatomic.h>
#include <stdint.h>

#include "eip_scanner_config.h"
#include "eip_scanner_shm_layout_public.h"
#include "softplc/ipc/spsc_ring.h"

#ifdef __cplusplus
#error "This header is C-only by design: the C++ backend must not touch the rings."
#endif

/** "EIS1". */
#define EIP_SCANNER_SHM_MAGIC       0x45495331u
#define EIP_SCANNER_SHM_ABI_VERSION 1u

#define EIP_SCANNER_SHM_NAME_MAX 128
#define EIP_SCANNER_INSTANCE_MAX  32

/* Per-device health byte values, as seen by a POU in the input image, come
 * from eip_scanner_shm_layout_public.h - the C++ backends report them too. */

/**
 * @brief Input image layout: the health block precedes the I/O data.
 *
 * `%I[i_offset .. i_offset + device_count)`  one health byte per device
 * `%I[i_offset + health_bytes .. ]`          the concatenated T->O slices
 *
 * Health first so that its position does not move when a device's image size
 * is retuned - only the data after it shifts.
 */
#define EIP_SCANNER_HEALTH_BYTES(device_count) ((uint32_t)(device_count))

#define EIP_SCANNER_DEFAULT_EXCHANGE_TIMEOUT_US 5000u
#define EIP_SCANNER_DEFAULT_TIMEOUT_THRESHOLD      3u

typedef struct eip_scanner_status {
    _Atomic uint32_t adapter_state;      /**< plc_adapter_state_t          */
    _Atomic uint32_t devices_online;     /**< how many are connected       */
    _Atomic uint64_t cycles;             /**< service loop iterations      */
    _Atomic uint64_t forward_opens;      /**< successful ForwardOpen count */
    _Atomic uint64_t connection_losses;  /**< connections dropped/timed out*/
    _Atomic uint64_t last_error;
} eip_scanner_status_t;

typedef struct eip_scanner_shm {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t layout_bytes;

    uint32_t device_count;
    uint32_t health_bytes;    /**< == device_count; explicit so the core need
                               *   not re-derive the input layout            */
    uint32_t o2t_total_bytes; /**< PLC -> field, the aggregate O->T image    */
    uint32_t t2o_total_bytes; /**< field -> PLC, the aggregate T->O image    */
    uint32_t input_bytes;     /**< health_bytes + t2o_total_bytes            */
    uint32_t reserved[7];

    eip_scanner_status_t status;

    plc_spsc_ring_t req;  /**< core -> scanner: the aggregate O->T image */
    plc_spsc_ring_t rsp;  /**< scanner -> core: health bytes then T->O   */
} eip_scanner_shm_t;

void eip_scanner_shm_name(char *buf, size_t buf_len, const char *instance);
void eip_scanner_sem_req_name(char *buf, size_t buf_len, const char *instance);
void eip_scanner_sem_rsp_name(char *buf, size_t buf_len, const char *instance);

#endif /* SOFTPLC_EIP_SCANNER_SHM_LAYOUT_H */
