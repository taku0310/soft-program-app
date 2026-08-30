/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_adapter_main.c
 * @brief The EtherNet/IP adapter process.
 *
 * Runs on the other side of the crash-containment boundary from the PLC core.
 * Its entire job is: attach to the region the core created, then loop on
 * "wait for a request frame, hand the outputs to the stack, take a snapshot of
 * the inputs, publish a reply frame".
 *
 * Two properties are deliberate.
 *
 * It never creates IPC objects, only attaches - so a crash here leaves nothing
 * to clean up and the core keeps ownership of the namespace.  And it does not
 * try to survive anything: on a fatal error it exits non-zero and lets the
 * supervisor or the container runtime decide what happens next.  Restart
 * policy is out of scope here on purpose; the core needs only to keep
 * scanning, which it does by falling through to its failsafe.
 *
 * Startup ordering is not coordinated either.  The adapter may come up before
 * or after the core; if the region is not there yet it retries, and the core's
 * exchanges simply time out until this process is serving.
 */
#include "eip_backend.h"
#include "eip_shm_layout.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "softplc/ipc/shm.h"
#include "softplc/plc_log.h"
#include "softplc/protocol_adapter.h"

#define ATTACH_RETRY_US   200000u  /* 200 ms between attach attempts */
#define SERVICE_WAIT_US   500000u  /* doorbell wait; also the exit-check tick */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static const char *env_str(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

static uint32_t env_u32(const char *key, uint32_t fallback) {
    const char *v = getenv(key);
    if (!v || !*v) return fallback;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    return (end == v || n == 0 || n > 0xFFFFFFFFul) ? fallback : (uint32_t)n;
}

static void sleep_us(uint32_t us) {
    struct timespec ts = { .tv_sec = us / 1000000u,
                           .tv_nsec = (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

/**
 * Attach to the core's region, retrying until it exists or we are stopped.
 *
 * The magic word is written last by the core, so seeing it means the whole
 * layout is published.  Checking it, the ABI version and the struct size here
 * turns "the two binaries were built from different headers" into a clear
 * start-up failure instead of a corrupt exchange later.
 */
static plc_status_t attach(const char *instance, plc_shm_t *shm, eip_shm_t **map) {
    char name[EIP_SHM_NAME_MAX];
    eip_shm_name(name, sizeof(name), instance);

    /* plc_shm_attach() logs the non-ENOENT failures, but this loop runs every
     * 200 ms and the interesting ones - EACCES from a uid or capability
     * mismatch - do not clear up on their own.  Report each distinct status
     * once rather than five lines a second for the life of the container. */
    plc_status_t reported = PLC_OK;

    for (;;) {
        if (g_stop) return PLC_ERR_STATE;

        plc_status_t st = plc_shm_attach(shm, name, sizeof(eip_shm_t));
        if (st != PLC_OK && st != reported) {
            switch (st) {
                case PLC_ERR_NOTFOUND:
                    PLC_LOG_INFO("waiting for the PLC core to publish %s", name);
                    break;
                case PLC_ERR_PROTO:
                    /* Not errno-backed: the region exists but is too small for
                     * this build's layout. */
                    PLC_LOG_ERR("%s is smaller than the %zu byte layout this "
                                "binary expects - core and adapter built from "
                                "different headers?", name, sizeof(eip_shm_t));
                    break;
                default:
                    PLC_LOG_ERR("cannot attach to %s: %s; still retrying "
                                "(a uid or capability mismatch with the PLC "
                                "core will not clear on its own)",
                                name, strerror(errno));
                    break;
            }
            reported = st;
        }
        if (st == PLC_OK) {
            eip_shm_t *m = shm->base;
            if (m->magic == EIP_SHM_MAGIC &&
                m->abi_version == EIP_SHM_ABI_VERSION &&
                m->layout_bytes == sizeof(eip_shm_t)) {
                *map = m;
                PLC_LOG_INFO("attached to %s (in=%u out=%u)",
                             name, m->input_bytes, m->output_bytes);
                return PLC_OK;
            }
            if (m->magic != 0 && m->magic != EIP_SHM_MAGIC) {
                PLC_LOG_ERR("%s: bad magic 0x%08x - core and adapter built "
                            "from different headers?", name, m->magic);
                plc_shm_close(shm);
                return PLC_ERR_PROTO;
            }
            if (m->magic == EIP_SHM_MAGIC) {
                PLC_LOG_ERR("%s: ABI %u/%u bytes, expected %u/%zu",
                            name, m->abi_version, m->layout_bytes,
                            EIP_SHM_ABI_VERSION, sizeof(eip_shm_t));
                plc_shm_close(shm);
                return PLC_ERR_PROTO;
            }
            plc_shm_close(shm);   /* still being initialised; try again */
        }
        sleep_us(ATTACH_RETRY_US);
    }
}

int main(int argc, char **argv) {
    plc_log_init("eip-adapter");

    const char *instance = (argc > 1) ? argv[1]
                                      : env_str("SOFTPLC_INSTANCE", "default");
    const char *iface    = (argc > 2) ? argv[2]
                                      : env_str("SOFTPLC_EIP_INTERFACE", "eth0");

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* A core that exits first leaves us writing to an unmapped doorbell.
     * Ignoring SIGPIPE keeps that a return code we can act on. */
    signal(SIGPIPE, SIG_IGN);

    plc_shm_t  shm = { .fd = -1 };
    eip_shm_t *map = NULL;
    if (attach(instance, &shm, &map) != PLC_OK) {
        PLC_LOG_ERR("cannot attach to instance '%s'", instance);
        return EXIT_FAILURE;
    }

    char req_name[EIP_SHM_NAME_MAX], rsp_name[EIP_SHM_NAME_MAX];
    eip_sem_req_name(req_name, sizeof(req_name), instance);
    eip_sem_rsp_name(rsp_name, sizeof(rsp_name), instance);

    /* The core creates both doorbells before it publishes the magic word, so
     * having attached above means they exist; anything failing here is a real
     * error rather than a startup race, and is worth exiting on. */
    sem_t *sem_req = NULL, *sem_rsp = NULL;
    if (plc_sem_attach(&sem_req, req_name) != PLC_OK ||
        plc_sem_attach(&sem_rsp, rsp_name) != PLC_OK) {
        PLC_LOG_ERR("cannot open the doorbells for instance '%s': %s",
                    instance, strerror(errno));
        plc_sem_close(sem_req, req_name, 0);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    const eip_backend_t *backend = eip_backend_get();
    const eip_backend_config_t cfg = {
        .interface         = iface,
        .produced_assembly = map->produced_assembly ? map->produced_assembly
                                                    : EIP_DEFAULT_PRODUCED_ASSEMBLY,
        .consumed_assembly = map->consumed_assembly ? map->consumed_assembly
                                                    : EIP_DEFAULT_CONSUMED_ASSEMBLY,
        .config_assembly   = env_u32("SOFTPLC_EIP_CONFIG_ASSEMBLY",
                                     EIP_DEFAULT_CONFIG_ASSEMBLY),
        .output_bytes      = map->output_bytes,
        .input_bytes       = map->input_bytes,
    };

    atomic_store(&map->status.adapter_state, PLC_ADAPTER_OPENING);

    if (backend->init(&cfg) != PLC_OK) {
        PLC_LOG_ERR("backend '%s' failed to start", backend->name);
        atomic_store(&map->status.adapter_state, PLC_ADAPTER_FAULTED);
        backend->shutdown();
        plc_sem_close(sem_req, req_name, 0);
        plc_sem_close(sem_rsp, rsp_name, 0);
        plc_shm_close(&shm);
        return EXIT_FAILURE;
    }

    atomic_store(&map->status.adapter_state, PLC_ADAPTER_ONLINE);
    PLC_LOG_INFO("serving instance '%s' with backend '%s' on '%s'",
                 instance, backend->name, iface);

    /* --- service loop ---------------------------------------------------- */

    plc_ipc_frame_t frame;
    uint8_t reply[PLC_IPC_MAX_FRAME_BYTES];

    while (!g_stop) {
        /* Bounded wait rather than an indefinite one, so a stopped core does
         * not leave this process unkillable-by-design and SIGTERM is still
         * observed within one tick. */
        const plc_status_t w = plc_sem_wait_timeout(sem_req, SERVICE_WAIT_US);
        if (w == PLC_ERR_TIMEOUT) continue;
        if (w != PLC_OK) {
            PLC_LOG_ERR("doorbell wait failed; exiting");
            break;
        }

        plc_status_t st = plc_spsc_pop(&map->req, &frame, PLC_IPC_MAX_FRAME_BYTES);
        if (st == PLC_ERR_AGAIN) continue;            /* spurious wake */
        if (st != PLC_OK) {
            /* A malformed frame is the core's problem to notice; drop it and
             * keep serving rather than taking the whole adapter down. */
            PLC_LOG_WARN("dropping malformed request frame");
            atomic_store(&map->status.last_error, (uint64_t)(int64_t)st);
            continue;
        }

        backend->publish_outputs(frame.data, frame.len);
        const size_t n = backend->fetch_inputs(reply, sizeof(reply));

        /* Echo the sequence number so the core can tell our reply apart from
         * a late one it already gave up on. */
        st = plc_spsc_push(&map->rsp, frame.seq, reply, (uint32_t)n);
        if (st != PLC_OK) {
            /* The core is not draining: it has stopped, or it is timing out
             * and discarding.  Drop the reply - it would be stale anyway. */
            plc_spsc_drain(&map->rsp);
            atomic_store(&map->status.last_error, (uint64_t)(int64_t)st);
        } else {
            sem_post(sem_rsp);
        }

        atomic_fetch_add(&map->status.cycles, 1);
        atomic_store(&map->status.io_connections, backend->io_connections());
    }

    PLC_LOG_INFO("stopping");
    atomic_store(&map->status.adapter_state, PLC_ADAPTER_CLOSED);
    backend->shutdown();
    /* owner=0 throughout: the core created these and the core unlinks them. */
    plc_sem_close(sem_req, req_name, 0);
    plc_sem_close(sem_rsp, rsp_name, 0);
    plc_shm_close(&shm);
    return EXIT_SUCCESS;
}
