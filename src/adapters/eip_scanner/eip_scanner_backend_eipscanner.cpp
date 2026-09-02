/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_backend_eipscanner.cpp
 * @brief EtherNet/IP Scanner backend built on EIPScanner (nimbuscontrols).
 *
 * We are the **originator** here, which inverts the CIP direction labels
 * against the adapter role:
 *
 *     PLC %Q  ->  O->T  (we send to each remote device)
 *     PLC %I  <-  T->O  (each device sends to us)
 *
 * EIPScanner is single-threaded and caller-driven: `handleConnections()` runs
 * a select and returns, spawning nothing. So the split here is simply that the
 * service loop's stack thread owns every EIPScanner object and calls poll(),
 * while the IPC thread only ever calls exchange_images(). The two meet at one
 * mutex around a pair of byte vectors - the same narrow boundary the OpENer
 * backend uses, for the same reason.
 *
 * Per-device failsafe is applied *here*, per device, under that device's own
 * policy from the device table. The core's adapter-level failsafe is a coarser
 * thing that only fires when this whole process stops answering.
 */
#include "eip_scanner_backend.h"
#include "eip_scanner_shm_layout_public.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConnectionManager.h"
#include "SessionInfo.h"
#include "cip/connectionManager/ConnectionParameters.h"
#include "cip/connectionManager/NetworkConnectionParams.h"
#include "utils/Logger.h"

using eipScanner::ConnectionManager;
using eipScanner::IOConnection;
using eipScanner::SessionInfo;
using eipScanner::cip::connectionManager::ConnectionParameters;
using eipScanner::cip::connectionManager::NetworkConnectionParams;

namespace {

constexpr uint16_t kEipPort = 0xAF12;  /* 44818 */

struct Device {
    eip_scanner_device_t   cfg{};
    std::shared_ptr<SessionInfo> session;
    IOConnection::WPtr     io;
    bool                   connected = false;
    /** Last image received while connected; what HOLD reproduces. */
    std::vector<uint8_t>   last_good;
    /** Latest received image, published to the IPC thread under g_lock. */
    std::vector<uint8_t>   latest;
    std::vector<uint8_t>   to_send;
    /** Retry gate: a powered-down drive must not be reconnected every poll. */
    std::chrono::steady_clock::time_point next_retry{};
};

std::mutex             g_lock;      /* guards Device::latest / ::to_send      */
eip_scanner_config_t   g_cfg{};
std::vector<Device>    g_devices;
std::unique_ptr<ConnectionManager> g_cm;
uint64_t               g_forward_opens = 0;
uint64_t               g_losses        = 0;

constexpr auto kReconnectInterval = std::chrono::seconds(2);

/** Build the Class 1 connection parameters for one device. */
ConnectionParameters make_params(const eip_scanner_device_t &d) {
    ConnectionParameters p;
    /* Connection path: 0x20 0x04 = Assembly class, then config / O2T / T2O
     * instances as 8-bit segments (0x24) with 0x2C separators - the encoding
     * upstream's own example uses. */
    p.connectionPath = {0x20, 0x04,
                        0x24, static_cast<uint8_t>(d.config_assembly),
                        0x2C, static_cast<uint8_t>(d.o2t_assembly),
                        0x2C, static_cast<uint8_t>(d.t2o_assembly)};
    p.o2tRealTimeFormat = true;
    p.originatorVendorId = 1;
    p.originatorSerialNumber = 0x534F4654;  /* "SOFT" */

    p.o2tNetworkConnectionParams |= NetworkConnectionParams::P2P;
    p.o2tNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
    p.o2tNetworkConnectionParams |= d.o2t_bytes;

    p.t2oNetworkConnectionParams |= NetworkConnectionParams::P2P;
    p.t2oNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
    p.t2oNetworkConnectionParams |= d.t2o_bytes;

    p.o2tRPI = d.o2t_rpi_us;
    p.t2oRPI = d.t2o_rpi_us;
    p.transportTypeTrigger |= NetworkConnectionParams::CLASS1;
    return p;
}

/** Open (or reopen) one device's connection.  Failure is not fatal: a scanner
 *  whose third drive is powered down must still run the other three. */
void connect_device(size_t index) {
    Device &dev = g_devices[index];
    const auto now = std::chrono::steady_clock::now();
    if (now < dev.next_retry) return;
    dev.next_retry = now + kReconnectInterval;

    try {
        dev.session = std::make_shared<SessionInfo>(dev.cfg.address, kEipPort);
    } catch (const std::exception &e) {
        eipScanner::utils::Logger(eipScanner::utils::LogLevel::WARNING)
            << "session to " << dev.cfg.address << " failed: " << e.what();
        dev.session.reset();
        return;
    }

    auto io = g_cm->forwardOpen(dev.session, make_params(dev.cfg));
    auto ptr = io.lock();
    if (!ptr) {
        eipScanner::utils::Logger(eipScanner::utils::LogLevel::WARNING)
            << "ForwardOpen to " << dev.cfg.address << " rejected";
        dev.session.reset();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_lock);
        ptr->setDataToSend(dev.to_send);
    }

    ptr->setReceiveDataListener(
        [index](auto /*realTimeHeader*/, auto /*sequence*/,
                const std::vector<uint8_t> &data) {
            /* Runs on the stack thread inside handleConnections(). */
            Device &d = g_devices[index];
            std::lock_guard<std::mutex> lk(g_lock);
            const size_t n = std::min<size_t>(data.size(), d.latest.size());
            if (n) std::memcpy(d.latest.data(), data.data(), n);
            if (n < d.latest.size()) {
                std::memset(d.latest.data() + n, 0, d.latest.size() - n);
            }
            d.last_good = d.latest;
        });

    ptr->setCloseListener([index]() {
        Device &d = g_devices[index];
        d.connected = false;
        d.next_retry = std::chrono::steady_clock::now() + kReconnectInterval;
        g_losses++;
        eipScanner::utils::Logger(eipScanner::utils::LogLevel::WARNING)
            << "connection to " << d.cfg.address << " closed";
    });

    dev.io = io;
    dev.connected = true;
    g_forward_opens++;
    eipScanner::utils::Logger(eipScanner::utils::LogLevel::INFO)
        << "connected to " << dev.cfg.address
        << " o2t=" << dev.cfg.o2t_bytes << "B t2o=" << dev.cfg.t2o_bytes << "B";
}

/* --- backend interface --------------------------------------------------- */

plc_status_t scanner_init(const eip_scanner_config_t *cfg) {
    if (!cfg || cfg->device_count == 0) return PLC_ERR_INVAL;
    g_cfg = *cfg;

    /* The stack's own log level, separate from ours: diagnosing a connection
     * that will not establish needs its ForwardOpen and connection-lifecycle
     * lines, and hard-coding WARNING makes that impossible in the field. */
    {
        eipScanner::utils::LogLevel lvl = eipScanner::utils::LogLevel::WARNING;
        if (const char *v = std::getenv("SOFTPLC_SCANNER_STACK_LOG")) {
            if      (std::strcmp(v, "debug") == 0) lvl = eipScanner::utils::LogLevel::DEBUG;
            else if (std::strcmp(v, "info")  == 0) lvl = eipScanner::utils::LogLevel::INFO;
            else if (std::strcmp(v, "error") == 0) lvl = eipScanner::utils::LogLevel::ERROR;
        }
        eipScanner::utils::Logger::setLogLevel(lvl);
    }

    g_cm = std::make_unique<ConnectionManager>();
    g_devices.clear();
    g_devices.resize(cfg->device_count);

    for (uint32_t i = 0; i < cfg->device_count; ++i) {
        Device &d = g_devices[i];
        d.cfg = cfg->devices[i];
        d.latest.assign(d.cfg.t2o_bytes, 0);
        d.last_good.assign(d.cfg.t2o_bytes, 0);
        d.to_send.assign(d.cfg.o2t_bytes, 0);
        connect_device(i);
    }

    /* Deliberately PLC_OK even if every ForwardOpen failed. The devices may
     * simply not be powered yet, and poll() keeps retrying; failing here would
     * take the process down and hand the core a dead peer instead of a live
     * one reporting every device offline. */
    return PLC_OK;
}

void scanner_shutdown() {
    if (g_cm) {
        for (auto &d : g_devices) {
            if (auto ptr = d.io.lock()) g_cm->forwardClose(d.session, d.io);
        }
        g_cm.reset();
    }
    g_devices.clear();
}

void scanner_poll(uint32_t budget_us) {
    if (!g_cm) return;

    /* Push the freshest outputs into each live connection before servicing. */
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto &d : g_devices) {
            if (!d.connected) continue;
            if (auto ptr = d.io.lock()) ptr->setDataToSend(d.to_send);
            else d.connected = false;
        }
    }

    /* Only service when something is actually open.
     *
     * Two reasons, and the first is not optional. Upstream's
     * BaseSocket::select() begins with *std::max_element(sockets...) and
     * dereferences it unconditionally, so calling handleConnections() with an
     * empty socket list is undefined behaviour - which is exactly the state
     * this process is in whenever every device is unreachable.
     *
     * Second, it would spin. With nothing to select on there is no sleep in
     * the loop, so this thread would take and release the mutex the IPC thread
     * needs at full speed, burn a core, and on a small host starve exchanges
     * past their budget. A scanner whose devices are all down must idle, not
     * spin. */
    if (g_cm->hasOpenConnections()) {
        g_cm->handleConnections(std::chrono::milliseconds(budget_us / 1000 + 1));
    } else {
        std::this_thread::sleep_for(std::chrono::microseconds(budget_us));
    }

    for (size_t i = 0; i < g_devices.size(); ++i) {
        if (!g_devices[i].connected) connect_device(i);
    }
}

size_t scanner_exchange(const uint8_t *o2t, size_t o2t_len,
                        uint8_t *health, size_t health_len,
                        uint8_t *t2o, size_t t2o_cap) {
    std::lock_guard<std::mutex> lk(g_lock);

    const size_t total = std::min<size_t>(g_cfg.total_t2o_bytes, t2o_cap);

    for (size_t i = 0; i < g_devices.size(); ++i) {
        Device &d = g_devices[i];
        const eip_scanner_device_t &c = d.cfg;

        /* Stage this device's outputs; poll() hands them to the connection. */
        if (o2t && c.o2t_offset + c.o2t_bytes <= o2t_len) {
            std::memcpy(d.to_send.data(), o2t + c.o2t_offset, c.o2t_bytes);
        }

        if (c.t2o_offset + c.t2o_bytes > total) continue;
        uint8_t *slice = t2o + c.t2o_offset;

        if (d.connected) {
            std::memcpy(slice, d.latest.data(), c.t2o_bytes);
            if (i < health_len) health[i] = EIP_DEVICE_ONLINE;
        } else {
            /* Per-device failsafe under this device's own policy. The rest of
             * the image is untouched: one drive dropping must not disturb
             * three healthy ones. */
            if (c.failsafe_policy == PLC_FAILSAFE_CLEAR) {
                std::memset(slice, 0, c.t2o_bytes);
            } else {
                std::memcpy(slice, d.last_good.data(), c.t2o_bytes);
            }
            if (i < health_len) {
                health[i] = g_forward_opens ? EIP_DEVICE_FAILSAFE
                                            : EIP_DEVICE_OFFLINE;
            }
        }
    }
    return total;
}

uint32_t scanner_online() {
    uint32_t n = 0;
    for (const auto &d : g_devices) if (d.connected) n++;
    return n;
}

uint64_t scanner_forward_opens() { return g_forward_opens; }
uint64_t scanner_losses()        { return g_losses; }

const eip_scanner_backend_t kEipScanner = {
    "eipscanner",
    scanner_init,
    scanner_shutdown,
    scanner_poll,
    scanner_exchange,
    scanner_online,
    scanner_forward_opens,
    scanner_losses,
};

}  // namespace

extern "C" const eip_scanner_backend_t *eip_scanner_backend_get(void) {
    return &kEipScanner;
}
