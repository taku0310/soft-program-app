/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_backend.h
 * @brief The scanner process's view of "an EtherNet/IP originator stack".
 *
 * This interface exists for a second reason beyond the one eip_backend.h has.
 * EIPScanner is C++20; the rings and the shared-memory layout are C11 and use
 * `_Atomic`, which C++ cannot portably include. Rather than fight that with a
 * layout-compatible shim, the boundary is drawn here: the service loop stays
 * **C** and owns everything to do with shared memory, and the C++ translation
 * unit sees only this header and never a ring. `eip_scanner_shm_layout.h`
 * enforces it with an #error on __cplusplus.
 *
 * Threading: the backend is driven from the service loop's stack thread; the
 * IPC thread only calls exchange_images(). Serialisation is the backend's job
 * and must stay a memcpy - it is inside the PLC's scan budget.
 */
#ifndef SOFTPLC_EIP_SCANNER_BACKEND_H
#define SOFTPLC_EIP_SCANNER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "eip_scanner_config.h"
#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eip_scanner_backend {
    const char *name;

    /** Bring up sessions and open a Class 1 connection per device. Returns
     *  ::PLC_OK even when some devices are unreachable: a scanner whose third
     *  drive is powered down must still run the other three. */
    plc_status_t (*init)(const eip_scanner_config_t *cfg);

    void (*shutdown)(void);

    /** Drive the stack for up to @p budget_us. Called in a loop by the stack
     *  thread; this is where connections are serviced and reopened. */
    void (*poll)(uint32_t budget_us);

    /**
     * @brief One cyclic image swap, called from the IPC thread.
     *
     * @param o2t       aggregate O->T image from the PLC (%Q), device order.
     * @param o2t_len   its length.
     * @param health    receives one ::EIP_DEVICE_ONLINE style byte per device.
     * @param t2o       receives the aggregate T->O image (%I data section),
     *                  with each device's slice already failsafed per that
     *                  device's own policy if its connection is down.
     * @return bytes written into @p t2o.
     */
    size_t (*exchange_images)(const uint8_t *o2t, size_t o2t_len,
                              uint8_t *health, size_t health_len,
                              uint8_t *t2o, size_t t2o_cap);

    uint32_t (*devices_online)(void);
    uint64_t (*forward_opens)(void);
    uint64_t (*connection_losses)(void);
} eip_scanner_backend_t;

/** The backend this binary was built with.  Never NULL. */
const eip_scanner_backend_t *eip_scanner_backend_get(void);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_EIP_SCANNER_BACKEND_H */
