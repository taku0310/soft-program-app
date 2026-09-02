/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/adapter_registry.h"

#include <string.h>

/* A fixed table rather than a linked list: the set of protocols in an image is
 * decided at build time, and a static table keeps registration allocation-free
 * and therefore usable before the runtime exists. */
static const plc_adapter_factory_t *g_factories[PLC_ADAPTER_REGISTRY_MAX];
static size_t g_count;

plc_status_t plc_adapter_registry_add(const plc_adapter_factory_t *f) {
    if (!f || !f->protocol || !f->create) return PLC_ERR_INVAL;
    if (plc_adapter_registry_find(f->protocol)) return PLC_ERR_STATE;
    if (g_count >= PLC_ADAPTER_REGISTRY_MAX) return PLC_ERR_NOMEM;
    g_factories[g_count++] = f;
    return PLC_OK;
}

const plc_adapter_factory_t *plc_adapter_registry_find(const char *protocol) {
    if (!protocol) return NULL;
    for (size_t i = 0; i < g_count; ++i) {
        if (strcmp(g_factories[i]->protocol, protocol) == 0) return g_factories[i];
    }
    return NULL;
}

size_t plc_adapter_registry_count(void) { return g_count; }

const plc_adapter_factory_t *plc_adapter_registry_at(size_t index) {
    return (index < g_count) ? g_factories[index] : NULL;
}

void plc_adapter_registry_reset(void) {
    memset(g_factories, 0, sizeof(g_factories));
    g_count = 0;
}
