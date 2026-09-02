/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_main.c
 * @brief The EtherNet/IP Scanner (originator) process.
 *
 * Deliberately C, not C++, even though the stack behind it is C++20: this file
 * owns the shared memory and the `_Atomic` rings, and keeping it in C means
 * the two languages never have to agree on an atomic's layout. Everything
 * protocol-specific is behind eip_scanner_backend.h.
 *
 * Two threads, for the same reason the OpENer adapter has two: the stack wants
 * to be serviced continuously at its own RPI while the PLC's exchange must
 * return inside a scan budget. The stack thread calls poll(); this thread
 * answers the core. They meet only in the backend's exchange_images().
 *
 * Like the adapter process, it creates nothing and restarts nothing. The core
 * owns the IPC objects; the supervisor owns the lifecycle.
 */
#include "eip_scanner_backend.h"
#include "eip_scanner_shm_layout.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "softplc/ipc/shm.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

#define ATTACH_RETRY_US 200000u
#define SERVICE_WAIT_US 500000u
#define POLL_BUDGET_US    2000u

static volatile sig_atomic_t g_stop;
static const eip_scanner_backend_t *g_backend;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static const char *env_str(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

static void sleep_us(uint32_t us) {
    struct timespec ts = { .tv_sec = us / 1000000u,
                           .tv_nsec = (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

/** Services the originator stack continuously, independent of the PLC scan. */
static void *stack_thread(void *arg) {
    (void)arg;
    while (!g_stop) g_backend->poll(POLL_BUDGET_US);
    return NULL;
}

static plc_status_t attach(const char *instance, plc_shm_t *shm,
                           eip_scanner_shm_t **map) {
    char name[EIP_SCANNER_SHM_NAME_MAX];
    eip_scanner_shm_name(name, sizeof(name), instance);

    /* Report each distinct failure once: this polls every 200 ms and the
     * interesting failures do not clear up on their own. */
    plc_status_t reported = PLC_OK;

    for (;;) {
        if (g_stop) return PLC_ERR_STATE;

        plc_status_t st = plc_shm_attach(shm, name, sizeof(eip_scanner_shm_t));
        if (st != PLC_OK && st != reported) {
            switch (st) {
                case PLC_ERR_NOTFOUND:
                    PLC_LOG_INFO("waiting for the PLC core to publish %s", name);
                    break;
                case PLC_ERR_PROTO:
                    PLC_LOG_ERR("%s is smaller than the %zu byte layout this "
                                "binary expects - core and scanner built from "
                                "different headers?",
                                name, sizeof(eip_scanner_shm_t));
                    break;
                default:
                    PLC_LOG_ERR("cannot attach to %s: %s; still retrying",
                                name, strerror(errno));
                    break;
            }
            reported = st;
        }

        if (st == PLC_OK) {
            eip_scanner_shm_t *m = shm->base;
            if (m->magic == EIP_SCANNER_SHM_MAGIC &&
                m->abi_version == EIP_SCANNER_SHM_ABI_VERSION &&
                m->layout_bytes == sizeof(eip_scanner_shm_t)) {
                *map = m;
                PLC_LOG_INFO("attached to %s (%u devices, o2t=%u t2o=%u)",
                             name, m->device_count,
                             m->o2t_total_bytes, m->t2o_total_bytes);
                return PLC_OK;
            }
            if (m->magic != 0 && m->magic != EIP_SCANNER_SHM_MAGIC) {
                PLC_LOG_ERR("%s: bad magic 0x%08x", name, m->magic);
                plc_shm_close(shm);
                return PLC_ERR_PROTO;
            }
            if (m->magic == EIP_SCANNER_SHM_MAGIC) {
                PLC_LOG_ERR("%s: ABI %u/%u, expected %u/%zu", name,
                            m->abi_version, m->layout_bytes,
                            EIP_SCANNER_SHM_ABI_VERSION, sizeof(eip_scanner_shm_t));
                plc_shm_close(shm);
                return PLC_ERR_PROTO;
            }
            plc_shm_close(shm);   /* still being initialised */
        }
        sleep_us(ATTACH_RETRY_US);
    }
}

int main(int argc, char **argv) {
    plc_log_init("eip-scanner");

    const char *instance = (argc > 1) ? argv[1]
                                      : env_str("SOFTPLC_INSTANCE", "default");
    const char *table_path = (argc > 2) ? argv[2]
        : env_str("SOFTPLC_SCANNER_DEVICES", "/etc/softplc/scanner-devices.conf");

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Read the same table the core read, so a mismatch surfaces as a size
     * check against the published layout rather than a truncated image. */
    eip_scanner_config_t table;
    char err[160];
    if (eip_scanner_config_load(&table, table_path, err, sizeof(err)) != PLC_OK) {
        PLC_LOG_ERR("device table %s: %s", table_path, err);
        return EXIT_FAILURE;
    }

    plc_shm_t          shm = { .fd = -1 };
    eip_scanner_shm_t *map = NULL;
    if (attach(instance, &shm, &map) != PLC_OK) {
        PLC_LOG_ERR("cannot attach to instance '%s'", instance);
        return EXIT_FAILURE;
    }

    if (map->device_count != table.device_count ||
        map->o2t_total_bytes != table.total_o2t_bytes ||
        map->t2o_total_bytes != table.total_t2o_bytes) {
        PLC_LOG_ERR("device table disagrees with the core: core has %u devices "
                    "(o2t=%u t2o=%u), this table has %u (o2t=%u t2o=%u) - "
                    "are both processes reading the same file?",
                    map->device_count, map->o2t_total_bytes, map->t2o_total_bytes,
                    table.device_count, table.total_o2t_bytes, table.total_t2o_bytes);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    char req_name[EIP_SCANNER_SHM_NAME_MAX], rsp_name[EIP_SCANNER_SHM_NAME_MAX];
    eip_scanner_sem_req_name(req_name, sizeof(req_name), instance);
    eip_scanner_sem_rsp_name(rsp_name, sizeof(rsp_name), instance);

    sem_t *sem_req = NULL, *sem_rsp = NULL;
    if (plc_sem_attach(&sem_req, req_name) != PLC_OK ||
        plc_sem_attach(&sem_rsp, rsp_name) != PLC_OK) {
        PLC_LOG_ERR("cannot open the doorbells for instance '%s': %s",
                    instance, strerror(errno));
        plc_sem_close(sem_req, req_name, 0);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    g_backend = eip_scanner_backend_get();
    atomic_store(&map->status.adapter_state, PLC_ADAPTER_OPENING);

    if (g_backend->init(&table) != PLC_OK) {
        PLC_LOG_ERR("backend '%s' failed to start", g_backend->name);
        atomic_store(&map->status.adapter_state, PLC_ADAPTER_FAULTED);
        g_backend->shutdown();
        plc_sem_close(sem_req, req_name, 0);
        plc_sem_close(sem_rsp, rsp_name, 0);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    pthread_t stack;
    if (pthread_create(&stack, NULL, stack_thread, NULL) != 0) {
        PLC_LOG_ERR("cannot start the stack thread: %s", strerror(errno));
        g_backend->shutdown();
        plc_sem_close(sem_req, req_name, 0);
        plc_sem_close(sem_rsp, rsp_name, 0);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    atomic_store(&map->status.adapter_state, PLC_ADAPTER_ONLINE);
    PLC_LOG_INFO("serving instance '%s' with backend '%s': %u device(s)",
                 instance, g_backend->name, table.device_count);

    /* --- service loop ---------------------------------------------------- */

    plc_ipc_frame_t frame;
    uint8_t reply[PLC_IPC_MAX_FRAME_BYTES];
    const uint32_t health_bytes = map->health_bytes;

    while (!g_stop) {
        const plc_status_t w = plc_sem_wait_timeout(sem_req, SERVICE_WAIT_US);
        if (w == PLC_ERR_TIMEOUT) continue;
        if (w != PLC_OK) {
            PLC_LOG_ERR("doorbell wait failed; exiting");
            break;
        }

        plc_status_t st = plc_spsc_pop(&map->req, &frame, PLC_IPC_MAX_FRAME_BYTES);
        if (st == PLC_ERR_AGAIN) continue;
        if (st != PLC_OK) {
            PLC_LOG_WARN("dropping malformed request frame");
            atomic_store(&map->status.last_error, (uint64_t)(int64_t)st);
            continue;
        }

        /* Reply layout: health block first, then the aggregate T->O data.
         * Health leads so its offsets do not move when an image size is
         * retuned. */
        memset(reply, 0, health_bytes);
        const size_t data = g_backend->exchange_images(
            frame.data, frame.len,
            reply, health_bytes,
            reply + health_bytes, sizeof(reply) - health_bytes);

        st = plc_spsc_push(&map->rsp, frame.seq, reply,
                           (uint32_t)(health_bytes + data));
        if (st != PLC_OK) {
            plc_spsc_drain(&map->rsp);
            atomic_store(&map->status.last_error, (uint64_t)(int64_t)st);
        } else {
            sem_post(sem_rsp);
        }

        atomic_fetch_add(&map->status.cycles, 1);
        atomic_store(&map->status.devices_online, g_backend->devices_online());
        atomic_store(&map->status.forward_opens, g_backend->forward_opens());
        atomic_store(&map->status.connection_losses, g_backend->connection_losses());
    }

    PLC_LOG_INFO("stopping");
    atomic_store(&map->status.adapter_state, PLC_ADAPTER_CLOSED);
    pthread_join(stack, NULL);
    g_backend->shutdown();
    plc_sem_close(sem_req, req_name, 0);
    plc_sem_close(sem_rsp, rsp_name, 0);
    plc_shm_close(&shm);
    return EXIT_SUCCESS;
}
