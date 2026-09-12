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

#include <stddef.h>
#include <string.h>

extern const plc_adapter_factory_t plc_loopback_adapter_factory;
#if SOFTPLC_WITH_EIP
extern const plc_adapter_factory_t plc_eip_adapter_factory;
#endif
#if SOFTPLC_WITH_EIP_SCANNER
extern const plc_adapter_factory_t plc_eip_scanner_adapter_factory;
#endif

void plc_adapter_register_builtins(void) {
    plc_adapter_registry_add(&plc_loopback_adapter_factory);
#if SOFTPLC_WITH_EIP
    plc_adapter_registry_add(&plc_eip_adapter_factory);
#endif
#if SOFTPLC_WITH_EIP_SCANNER
    plc_adapter_registry_add(&plc_eip_scanner_adapter_factory);
#endif
}

/* --- roles ---------------------------------------------------------------
 *
 * The two EtherNet/IP roles are mutually exclusive in one network namespace,
 * not by policy but by CIP: class 1 I/O uses UDP 2222 at *both* ends, so an
 * Adapter and a Scanner sharing a namespace receive each other's traffic.
 * That is why a role is one value rather than a set - and why running both
 * still means two namespaces, whatever the container count.
 *
 * SOFTPLC_ADAPTERS remains the escape hatch: it takes an arbitrary list and
 * overrides all of this, for the deployment that really does want two.
 */
static const struct {
    const char *role;
    const char *adapters;
} kRoles[] = {
    /* No fieldbus.  The loopback adapter still gives the program a process
     * image to drive, which is what makes this useful for logic testing
     * rather than merely inert. */
    { "none",    "loopback" },
#if SOFTPLC_WITH_EIP
    { "adapter", "ethernet-ip" },
#endif
#if SOFTPLC_WITH_EIP_SCANNER
    { "scanner", "ethernet-ip-scanner" },
#endif
};

const char *plc_adapter_role_adapters(const char *role) {
    if (!role) return NULL;
    for (size_t i = 0; i < sizeof(kRoles) / sizeof(kRoles[0]); ++i) {
        if (strcmp(kRoles[i].role, role) == 0) return kRoles[i].adapters;
    }
    return NULL;
}

const char *plc_adapter_role_at(size_t index) {
    return (index < sizeof(kRoles) / sizeof(kRoles[0])) ? kRoles[index].role : NULL;
}
