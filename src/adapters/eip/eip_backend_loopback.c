/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_backend_loopback.c
 * @brief Stack-free backend used when SOFTPLC_WITH_OPENER is OFF.
 *
 * Mirrors the produced assembly back into the consumed assembly, so the whole
 * IPC path - rings, doorbells, sequence matching, failsafe escalation - can be
 * exercised end to end without a network, a NIC, or the OpENer submodule.
 * That is what CI runs.  It is not an EtherNet/IP implementation and does not
 * pretend to be one: io_connections() reports a synthetic 1 so that the status
 * plumbing has something to carry.
 */
#include "eip_backend.h"

#include <pthread.h>
#include <string.h>

#include "softplc/ipc/spsc_ring.h"
#include "softplc/plc_log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_mirror[PLC_IPC_MAX_FRAME_BYTES];
static size_t          g_mirror_len;
static size_t          g_input_bytes;

static plc_status_t lb_init(const eip_backend_config_t *cfg) {
    pthread_mutex_lock(&g_lock);
    memset(g_mirror, 0, sizeof(g_mirror));
    g_mirror_len  = 0;
    g_input_bytes = cfg ? cfg->input_bytes : 0;
    pthread_mutex_unlock(&g_lock);
    PLC_LOG_WARN("built without OpENer: using the loopback backend, "
                 "no EtherNet/IP traffic will be served");
    return PLC_OK;
}

static void lb_shutdown(void) { }

static void lb_publish(const uint8_t *data, size_t len) {
    if (len > sizeof(g_mirror)) len = sizeof(g_mirror);
    pthread_mutex_lock(&g_lock);
    if (len) memcpy(g_mirror, data, len);
    g_mirror_len = len;
    pthread_mutex_unlock(&g_lock);
}

static size_t lb_fetch(uint8_t *data, size_t cap) {
    pthread_mutex_lock(&g_lock);
    size_t n = g_input_bytes ? g_input_bytes : g_mirror_len;
    if (n > cap) n = cap;
    if (n) {
        const size_t copy = (n < g_mirror_len) ? n : g_mirror_len;
        memcpy(data, g_mirror, copy);
        if (copy < n) memset(data + copy, 0, n - copy);
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static uint32_t lb_connections(void) { return 1; }

static const eip_backend_t kLoopbackBackend = {
    .name            = "loopback",
    .init            = lb_init,
    .shutdown        = lb_shutdown,
    .publish_outputs = lb_publish,
    .fetch_inputs    = lb_fetch,
    .io_connections  = lb_connections,
};

const eip_backend_t *eip_backend_get(void) { return &kLoopbackBackend; }
