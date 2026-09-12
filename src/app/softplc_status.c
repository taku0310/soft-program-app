/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file softplc_status.c
 * @brief `softplc --status`: the health of a running instance, as JSON.
 *
 * Acceptance check D1. Until this existed, everything the runtime knew about
 * itself was in its log: a human could read it, an orchestrator could not, and
 * neither could tell "connected" from "connected and receiving" without
 * reading prose. A liveness or readiness probe needs a number.
 *
 * It reads the shared memory the running instance publishes, read-only, and
 * prints one JSON object. It takes no locks, writes nothing, and asks the
 * running processes for nothing, so it cannot perturb the thing it measures -
 * which matters when the thing it measures is a control loop.
 *
 *   softplc --status [instance]
 *
 * Exit status: 0 when the instance is ONLINE, 3 when it is reachable but not
 * ONLINE, 4 when nothing is published under that name. That split is what
 * makes it usable as a container health check directly.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "softplc_status.h"
#include "softplc/protocol_adapter.h"
/* Only the roles this build carries: the shm name helpers live with their
 * adapters, so including the layouts unconditionally would not link. */
#if SOFTPLC_WITH_EIP
#include "../adapters/eip/eip_shm_layout.h"
#endif
#if SOFTPLC_WITH_EIP_SCANNER
#include "../adapters/eip_scanner/eip_scanner_shm_layout.h"
#endif

#define STATUS_OK        0
#define STATUS_NOT_READY 3
#define STATUS_ABSENT    4

/** Map a published region read-only. NULL if it is not there or is too small. */
static void *map_ro(const char *name, size_t want, size_t *got) {
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < want) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, want, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *got = (size_t)st.st_size;
    return p;
}

static const char *state_name(uint32_t s) {
    return plc_adapter_state_name((plc_adapter_state_t)s);
}

#if SOFTPLC_WITH_EIP
static int print_adapter(const char *instance) {
    char name[EIP_SHM_NAME_MAX];
    eip_shm_name(name, sizeof(name), instance);

    size_t got = 0;
    eip_shm_t *m = map_ro(name, sizeof(eip_shm_t), &got);
    if (!m) return STATUS_ABSENT;

    /* A region whose magic is not set yet is a core still publishing it; say
     * so rather than reporting zeroed counters as a healthy idle adapter. */
    const int ready = (m->magic == EIP_SHM_MAGIC);
    const uint32_t state = ready ? atomic_load(&m->status.adapter_state)
                                 : PLC_ADAPTER_CLOSED;
    const uint32_t abi = m->abi_version, layout = m->layout_bytes;
    const uint32_t conns = ready ? atomic_load(&m->status.io_connections) : 0u;
    const uint32_t in_bytes = m->input_bytes, out_bytes = m->output_bytes;
    const uint32_t produced = m->produced_assembly, consumed = m->consumed_assembly;
    const uint64_t cycles  = ready ? atomic_load(&m->status.cycles) : 0;
    const uint64_t writes  = ready ? atomic_load(&m->status.assembly_writes) : 0;
    const uint64_t lasterr = ready ? atomic_load(&m->status.last_error) : 0;
    munmap(m, sizeof(eip_shm_t));

    printf("{\"instance\":\"%s\",\"role\":\"adapter\",\"region\":\"%s\","
           "\"ready\":%s,\"abi_version\":%u,\"layout_bytes\":%u,"
           "\"state\":\"%s\",\"io_connections\":%u,"
           "\"input_bytes\":%u,\"output_bytes\":%u,"
           "\"produced_assembly\":%u,\"consumed_assembly\":%u,"
           "\"cycles\":%llu,\"assembly_writes\":%llu,\"last_error\":%llu}\n",
           instance, name, ready ? "true" : "false",
           abi, layout, state_name(state), conns,
           in_bytes, out_bytes, produced, consumed,
           (unsigned long long)cycles, (unsigned long long)writes,
           (unsigned long long)lasterr);

    /* An adapter with no connection is not ready, for the same reason a
     * scanner with no device is not: no I/O is flowing either way. The
     * adapter cannot make a controller connect to it, but "waiting for one"
     * and "serving one" are different states and a probe has to be able to
     * tell them apart - treating zero connections as healthy is the shape of
     * defect A2 found in the failsafe path. */
    return (ready && state == PLC_ADAPTER_ONLINE && conns > 0)
         ? STATUS_OK : STATUS_NOT_READY;
}
#endif /* SOFTPLC_WITH_EIP */

#if SOFTPLC_WITH_EIP_SCANNER
static int print_scanner(const char *instance) {
    char name[EIP_SCANNER_SHM_NAME_MAX];
    eip_scanner_shm_name(name, sizeof(name), instance);

    size_t got = 0;
    eip_scanner_shm_t *m = map_ro(name, sizeof(eip_scanner_shm_t), &got);
    if (!m) return STATUS_ABSENT;

    /* Everything is read out before anything is printed, and the region is
     * unmapped before the formatting: a snapshot cannot tear halfway through
     * a long printf, and nothing can reach a pointer that is no longer
     * mapped. The first version formatted straight out of the mapping and
     * read one field *after* the munmap - a use-after-unmap that segfaulted
     * every time the scanner role was asked for its status. */
    const int ready = (m->magic == EIP_SCANNER_SHM_MAGIC);
    const uint32_t state = ready ? atomic_load(&m->status.adapter_state)
                                 : PLC_ADAPTER_CLOSED;
    const uint32_t online = ready ? atomic_load(&m->status.devices_online) : 0u;
    const uint32_t abi = m->abi_version, layout = m->layout_bytes;
    const uint32_t devices = m->device_count;
    const uint32_t in_bytes = m->input_bytes, out_bytes = m->o2t_total_bytes;
    const uint64_t cycles   = ready ? atomic_load(&m->status.cycles) : 0;
    const uint64_t fopens   = ready ? atomic_load(&m->status.forward_opens) : 0;
    const uint64_t losses   = ready ? atomic_load(&m->status.connection_losses) : 0;
    const uint64_t ooo      = ready ? atomic_load(&m->status.out_of_order) : 0;
    const uint64_t lasterr  = ready ? atomic_load(&m->status.last_error) : 0;
    munmap(m, sizeof(eip_scanner_shm_t));

    printf("{\"instance\":\"%s\",\"role\":\"scanner\",\"region\":\"%s\","
           "\"ready\":%s,\"abi_version\":%u,\"layout_bytes\":%u,"
           "\"state\":\"%s\",\"devices\":{\"online\":%u,\"total\":%u},"
           "\"input_bytes\":%u,\"output_bytes\":%u,"
           "\"cycles\":%llu,\"forward_opens\":%llu,\"connection_losses\":%llu,"
           "\"out_of_order\":%llu,\"last_error\":%llu}\n",
           instance, name, ready ? "true" : "false",
           abi, layout, state_name(state), online, devices,
           in_bytes, out_bytes,
           (unsigned long long)cycles, (unsigned long long)fopens,
           (unsigned long long)losses, (unsigned long long)ooo,
           (unsigned long long)lasterr);

    /* Every device connected, or there is something to report. A scanner with
     * three of four drives up is not healthy, and a probe should not have to
     * parse prose to find that out. */
    const int all_up = ready && devices && online == devices;
    return (all_up && state == PLC_ADAPTER_ONLINE) ? STATUS_OK : STATUS_NOT_READY;
}
#endif /* SOFTPLC_WITH_EIP_SCANNER */

int softplc_print_status(const char *instance) {
    /* Whichever region exists. A container runs one role, and asking the
     * caller to know which one turns a health check into configuration. */
    int rc = STATUS_ABSENT;
#if SOFTPLC_WITH_EIP
    rc = print_adapter(instance);
    if (rc != STATUS_ABSENT) return rc;
#endif
#if SOFTPLC_WITH_EIP_SCANNER
    rc = print_scanner(instance);
    if (rc != STATUS_ABSENT) return rc;
#endif
    (void)rc;

    printf("{\"instance\":\"%s\",\"role\":null,\"ready\":false,"
           "\"error\":\"no published region for this instance\"}\n", instance);
    return STATUS_ABSENT;
}
