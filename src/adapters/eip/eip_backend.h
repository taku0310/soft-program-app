/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_backend.h
 * @brief The adapter process's view of "an EtherNet/IP stack".
 *
 * eip_adapter_main.c owns the IPC service loop and knows nothing about CIP.
 * Everything protocol-specific sits behind this five-call interface, which
 * exists for two reasons: it keeps the OpENer sources out of the IPC logic so
 * both can be reasoned about separately, and it lets the whole adapter process
 * be built and tested with no stack at all (SOFTPLC_WITH_OPENER=OFF), which is
 * what makes the tree buildable in CI and on a developer machine without the
 * submodule checked out.
 *
 * Threading: publish_outputs() and fetch_inputs() are called from the IPC
 * thread while the stack runs its own network loop elsewhere.  A backend is
 * responsible for its own serialisation, and must keep the critical section to
 * a memcpy - it is inside the PLC's scan budget.
 */
#ifndef SOFTPLC_EIP_BACKEND_H
#define SOFTPLC_EIP_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eip_backend_config {
    const char *interface;         /**< NIC the stack binds to, e.g. "eth0" */
    uint32_t    produced_assembly; /**< T->O instance: PLC outputs          */
    uint32_t    consumed_assembly; /**< O->T instance: PLC inputs           */
    uint32_t    config_assembly;
    uint32_t    output_bytes;      /**< size of the produced assembly       */
    uint32_t    input_bytes;       /**< size of the consumed assembly       */
} eip_backend_config_t;

typedef struct eip_backend {
    const char *name;

    /** Bring the stack up.  Must return only once it is serving. */
    plc_status_t (*init)(const eip_backend_config_t *cfg);

    /** Tear it down.  Must be safe to call after a failed init(). */
    void (*shutdown)(void);

    /** Copy @p len bytes of PLC output image into the produced assembly. */
    void (*publish_outputs)(const uint8_t *data, size_t len);

    /** Snapshot the consumed assembly.  @return bytes written into @p data. */
    size_t (*fetch_inputs)(uint8_t *data, size_t cap);

    /** Established CIP I/O connections; 0 means no scanner is talking to us. */
    uint32_t (*io_connections)(void);

    /**
     * @brief Is the connected originator asserting RUN?
     *
     * CIP puts a run/idle header on the O->T frame, and IDLE means "the data
     * in this frame is not valid, apply your idle action". OpENer reports the
     * bit but applies the payload either way, so without this the PLC would
     * keep driving outputs from data the controller has declared unusable.
     *
     * Returns 1 when the peer asserts RUN or when the question does not apply
     * (no run/idle header configured), 0 when it asserts IDLE. Optional: a
     * NULL pointer reads as "always RUN".
     */
    int (*peer_in_run)(void);
} eip_backend_t;

/** The backend this binary was built with.  Never NULL. */
const eip_backend_t *eip_backend_get(void);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_EIP_BACKEND_H */
