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
 * plumbing has something to carry, and two settings can force it to report no
 * connection or an idle peer so those paths are testable without hardware.
 */
#include "eip_backend.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "softplc/ipc/spsc_ring.h"
#include "softplc/plc_config.h"
#include "softplc/plc_log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_mirror[PLC_IPC_MAX_FRAME_BYTES];
static size_t          g_mirror_len;
static size_t          g_input_bytes;
/** The mirror has no originator, so what it counts is its own writes - which
 *  is the honest answer to "is anything filling the consumed assembly". */
static _Atomic uint64_t g_mirror_writes;

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
    atomic_fetch_add(&g_mirror_writes, 1);
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

/* Test-only, and the reason they exist: the two states in which a target
 * answers promptly with nothing valid to say - nobody connected, and an
 * originator asserting IDLE - are precisely the ones a mirror cannot reach on
 * its own, and precisely the ones that were wrong. Without a way to reach them
 * here, the fix would only ever be exercised against real hardware. Nothing in
 * a production image sets either. */
/* A boolean rather than a count: plc_cfg_u32() treats 0 as "unset" and hands
 * back the fallback, so a MIRROR_CONNECTIONS=0 knob would have reported one
 * connection and quietly passed a test written to prove the opposite. */
static uint32_t lb_connections(void) {
    return plc_cfg_bool("SOFTPLC_EIP_MIRROR_NO_CONNECTION", 0) ? 0u : 1u;
}

static int lb_peer_in_run(void) {
    return plc_cfg_bool("SOFTPLC_EIP_MIRROR_IDLE", 0) ? 0 : 1;
}

/** The mirror writes the image back every exchange, so every fetch is a
 *  write as far as anyone above can tell. */
static uint64_t lb_assembly_writes(void) {
    return atomic_load(&g_mirror_writes);
}

static const eip_backend_t kLoopbackBackend = {
    .name            = "loopback",
    .init            = lb_init,
    .shutdown        = lb_shutdown,
    .publish_outputs = lb_publish,
    .fetch_inputs    = lb_fetch,
    .io_connections  = lb_connections,
    .peer_in_run     = lb_peer_in_run,
    .assembly_writes = lb_assembly_writes,
};

const eip_backend_t *eip_backend_get(void) { return &kLoopbackBackend; }
