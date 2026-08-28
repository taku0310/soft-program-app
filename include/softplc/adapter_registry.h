/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file adapter_registry.h
 * @brief Factory registry that makes protocol stacks pluggable by name.
 *
 * This is the other half of the SIFB idea: the core never names a concrete
 * stack.  It asks the registry for "ethernet-ip" (or "modbus-tcp", or
 * "opc-ua") and gets back something that satisfies protocol_adapter.h.  Adding
 * a protocol is adding a factory and a build entry - no core change.
 */
#ifndef SOFTPLC_ADAPTER_REGISTRY_H
#define SOFTPLC_ADAPTER_REGISTRY_H

#include "softplc/protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_ADAPTER_REGISTRY_MAX 16

typedef struct plc_adapter_factory {
    /** Protocol family this factory serves, e.g. "ethernet-ip". */
    const char *protocol;
    /** One-line description for `softplc --list-adapters`. */
    const char *description;
    /** Allocate an unopened instance.  NULL on allocation failure. */
    plc_protocol_adapter_t *(*create)(void);
    /** Close if needed, then free.  Tolerates NULL. */
    void (*destroy)(plc_protocol_adapter_t *adapter);
} plc_adapter_factory_t;

/**
 * @brief Register @p f.
 *
 * @p f must have static storage duration; the registry keeps the pointer.
 * @return ::PLC_ERR_STATE if @p protocol is already registered.
 */
plc_status_t plc_adapter_registry_add(const plc_adapter_factory_t *f);

/** @return the factory for @p protocol, or NULL. */
const plc_adapter_factory_t *plc_adapter_registry_find(const char *protocol);

/** Number of registered factories. */
size_t plc_adapter_registry_count(void);

/** @return factory at @p index, or NULL when out of range. */
const plc_adapter_factory_t *plc_adapter_registry_at(size_t index);

/** Drop every registration.  Test helper; not for use while scanning. */
void plc_adapter_registry_reset(void);

/**
 * @brief Register every adapter compiled into this binary.
 *
 * Called once at start-up.  Which factories exist depends on build options,
 * which is exactly how a protocol gets added or left out of an image.
 */
void plc_adapter_register_builtins(void);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_ADAPTER_REGISTRY_H */
