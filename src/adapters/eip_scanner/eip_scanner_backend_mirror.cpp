/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_backend_mirror.cpp
 * @brief Stack-free backend used when SOFTPLC_WITH_EIPSCANNER is OFF.
 *
 * Mirrors each device's O->T slice back into its T->O slice and reports every
 * device online, so the whole IPC path - rings, doorbells, sequence matching,
 * aggregate image layout, health block, failsafe escalation - can be exercised
 * with no network and no submodule. This is what CI runs.
 *
 * It also supports injecting a device loss, which is the only way to test the
 * per-device failsafe without four real drives to unplug.
 *
 * Compiled as C++ rather than C purely so that the two backends present the
 * same translation-unit shape to the build; it uses nothing that needs it.
 */
#include "eip_scanner_backend.h"
#include "eip_scanner_shm_layout_public.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

std::mutex g_lock;
eip_scanner_config_t g_cfg;
std::vector<uint8_t> g_mirror;
/* Bitmask of devices forced offline, from SOFTPLC_SCANNER_MIRROR_DOWN.
 * Test-only: nothing sets it in a production image. */
uint32_t g_forced_down;
uint64_t g_losses;

plc_status_t mirror_init(const eip_scanner_config_t *cfg) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_cfg = *cfg;
    g_mirror.assign(cfg->total_t2o_bytes, 0);
    g_losses = 0;

    g_forced_down = 0;
    if (const char *v = std::getenv("SOFTPLC_SCANNER_MIRROR_DOWN")) {
        g_forced_down = (uint32_t)std::strtoul(v, nullptr, 0);
    }
    return PLC_OK;
}

void mirror_shutdown() {}
void mirror_poll(uint32_t) {}

size_t mirror_exchange(const uint8_t *o2t, size_t o2t_len,
                       uint8_t *health, size_t health_len,
                       uint8_t *t2o, size_t t2o_cap) {
    std::lock_guard<std::mutex> lk(g_lock);

    const size_t n = (g_cfg.total_t2o_bytes < t2o_cap)
                   ? g_cfg.total_t2o_bytes : t2o_cap;

    for (uint32_t i = 0; i < g_cfg.device_count; ++i) {
        const eip_scanner_device_t &d = g_cfg.devices[i];
        const bool down = (g_forced_down >> i) & 1u;

        uint8_t *slice = t2o + d.t2o_offset;
        if (d.t2o_offset + d.t2o_bytes > n) continue;

        if (down) {
            /* Per-device failsafe, exactly as the real backend must do it:
             * this device's slice only, under this device's own policy. */
            if (d.failsafe_policy == PLC_FAILSAFE_CLEAR) {
                std::memset(slice, 0, d.t2o_bytes);
            } else {
                std::memcpy(slice, g_mirror.data() + d.t2o_offset, d.t2o_bytes);
            }
            if (i < health_len) health[i] = EIP_DEVICE_FAILSAFE;
            continue;
        }

        /* Mirror this device's outputs into its inputs, truncating or
         * zero-extending to the configured T->O size. */
        const size_t copy = (d.o2t_bytes < d.t2o_bytes) ? d.o2t_bytes : d.t2o_bytes;
        if (d.o2t_offset + copy <= o2t_len && copy) {
            std::memcpy(slice, o2t + d.o2t_offset, copy);
        }
        if (copy < d.t2o_bytes) std::memset(slice + copy, 0, d.t2o_bytes - copy);

        std::memcpy(g_mirror.data() + d.t2o_offset, slice, d.t2o_bytes);
        if (i < health_len) health[i] = EIP_DEVICE_ONLINE;
    }
    return n;
}

uint32_t mirror_online() {
    std::lock_guard<std::mutex> lk(g_lock);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cfg.device_count; ++i) {
        if (!((g_forced_down >> i) & 1u)) n++;
    }
    return n;
}

uint64_t mirror_forward_opens() { return g_cfg.device_count; }
uint64_t mirror_losses()        { return g_losses; }

const eip_scanner_backend_t kMirror = {
    "mirror",
    mirror_init,
    mirror_shutdown,
    mirror_poll,
    mirror_exchange,
    mirror_online,
    mirror_forward_opens,
    mirror_losses,
};

}  // namespace

extern "C" const eip_scanner_backend_t *eip_scanner_backend_get(void) {
    return &kMirror;
}
