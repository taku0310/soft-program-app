/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file builtins.c
 * @brief The one place that knows which protocol stacks exist.
 *
 * Kept out of softplc_core deliberately.  The core links against
 * protocol_adapter.h and the registry and nothing else; if it named
 * `plc_eip_adapter_factory` it would have a build-time dependency on
 * EtherNet/IP, and the claim that protocols are pluggable would be false at
 * the link level however clean the headers looked.
 *
 * Adding Modbus/TCP or OPC UA means adding a factory, an extern here, and a
 * CMake entry.  Nothing in src/core/ changes.
 */
#include "softplc/adapter_registry.h"

extern const plc_adapter_factory_t plc_loopback_adapter_factory;
#if SOFTPLC_WITH_EIP
extern const plc_adapter_factory_t plc_eip_adapter_factory;
#endif

void plc_adapter_register_builtins(void) {
    plc_adapter_registry_add(&plc_loopback_adapter_factory);
#if SOFTPLC_WITH_EIP
    plc_adapter_registry_add(&plc_eip_adapter_factory);
#endif
}
