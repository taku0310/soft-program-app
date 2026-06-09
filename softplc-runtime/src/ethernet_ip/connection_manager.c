#include "connection_manager.h"

#include <stdio.h>
#include <string.h>

#include "cip_adapter.h"

static cip_connection_t g_connections[CIP_MAX_CONNECTIONS];
static int g_connection_count;

int cip_connection_manager_init(const char *config_path) {
    /* TODO: parse JSON config and populate g_connections.
     * For scaffold, initialize zero connections successfully. */
    (void)config_path;
    g_connection_count = 0;
    memset(g_connections, 0, sizeof(g_connections));
    fprintf(stderr, "[cip_mgr] initialized (%d connections)\n", g_connection_count);
    return 0;
}

void cip_connection_manager_sample_inputs(void) {
    for (int i = 0; i < g_connection_count; ++i) {
        cip_adapter_read(&g_connections[i]);
    }
}

void cip_connection_manager_sync_outputs(void) {
    for (int i = 0; i < g_connection_count; ++i) {
        cip_adapter_write(&g_connections[i]);
    }
}

void cip_connection_manager_shutdown(void) {
    for (int i = 0; i < g_connection_count; ++i) {
        cip_adapter_close(&g_connections[i]);
    }
    g_connection_count = 0;
}
