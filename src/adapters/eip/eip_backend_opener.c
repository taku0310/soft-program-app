/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_backend_opener.c
 * @brief EtherNet/IP backend built on OpENer (EIPStackGroup).
 *
 * OpENer implements a CIP *target* (an EtherNet/IP adapter), so an external
 * scanner owns the I/O connection.  That fixes the direction mapping:
 *
 *     PLC %Q  ->  produced assembly   (T->O on the wire)
 *     PLC %I  <-  consumed assembly   (O->T on the wire)
 *
 * OpENer is single-threaded and its internals are not reentrant, so the stack
 * gets a thread of its own and the only thing shared with the IPC thread is
 * the pair of assembly byte arrays we allocated ourselves and handed to
 * CreateAssemblyObject().  Those are guarded by one mutex held across a
 * memcpy and nothing else.  No OpENer function is ever called from the IPC
 * thread, which is what keeps this integration inside the stack's contract.
 *
 * The functions with external linkage below are OpENer's application
 * call-backs: the stack declares them in opener_api.h and expects the
 * application to define them.  This file replaces OpENer's sample application
 * wholesale - the sample is deliberately not compiled in, since it would
 * supply competing definitions of exactly these symbols.
 */
#include "eip_backend.h"
#include "eip_shm_layout.h"

#include <errno.h>
#include <net/if.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "appcontype.h"
#include "cipcommon.h"
#include "cipconnectionobject.h"
#include "cipethernetlink.h"
#include "cipidentity.h"
#include "cipqos.h"
#include "ciptcpipinterface.h"
#include "doublylinkedlist.h"
#include "generic_networkhandler.h"
#include "networkconfig.h"
#include "nvdata.h"
#include "opener_api.h"
#include "trace.h"

#include "softplc/ipc/spsc_ring.h"
#include "softplc/plc_log.h"

/** OpENer's own stop flag; its event loop polls this. */
extern volatile int g_end_stack;
volatile int g_end_stack = 0;

/* --- state shared with the IPC thread ------------------------------------ */

static pthread_mutex_t g_assembly_lock = PTHREAD_MUTEX_INITIALIZER;

/* Handed to CreateAssemblyObject(): OpENer's thread, and only OpENer's thread,
 * touches these.  The stack copies them in and out of the wire buffer at
 * moments we do not control, so letting the IPC thread write them directly
 * would race with a transmit already in progress. */
static uint8_t g_produced[PLC_IPC_MAX_FRAME_BYTES];  /* T->O: PLC outputs */
static uint8_t g_consumed[PLC_IPC_MAX_FRAME_BYTES];  /* O->T: PLC inputs  */
static uint8_t g_config[64];

/* The hand-off buffers.  The IPC thread only ever touches these, under the
 * lock; OpENer's thread moves data between them and the assemblies inside its
 * own call-backs, where it already knows no transmit is in flight. */
static uint8_t g_produced_pending[PLC_IPC_MAX_FRAME_BYTES];
static uint8_t g_consumed_snapshot[PLC_IPC_MAX_FRAME_BYTES];

static eip_backend_config_t g_cfg;
static pthread_t            g_stack_thread;
static int                  g_stack_running;
/* Set once CipStackInit() has succeeded.  shutdown() must be callable after a
 * failed init(), and ShutdownCipStack() is not safe on a stack that was never
 * initialised: EncapsulationShutDown() walks g_registered_sessions[], which is
 * only filled with kEipInvalidSocket (-1) by CipStackInit.  Before that it is
 * still the zero-filled BSS, so every slot would be closed as descriptor 0. */
static int                  g_stack_initialised;

/* Written by OpENer's thread from CheckIoConnectionEvent(), read by the IPC
 * thread for status only - never for a control decision. */
static _Atomic uint32_t g_io_connections;
static _Atomic uint64_t g_assembly_writes;

/* --- OpENer application call-backs --------------------------------------- */

EipStatus ApplicationInitialization(void) {
    CreateAssemblyObject((CipInstanceNum)g_cfg.produced_assembly,
                         g_produced, (EipUint16)g_cfg.output_bytes);
    CreateAssemblyObject((CipInstanceNum)g_cfg.consumed_assembly,
                         g_consumed, (EipUint16)g_cfg.input_bytes);
    CreateAssemblyObject((CipInstanceNum)g_cfg.config_assembly,
                         g_config, (EipUint16)sizeof(g_config));

    /* Heartbeat assemblies carry no data; they exist so that input-only and
     * listen-only connections have a valid O->T connection point. */
    CreateAssemblyObject(EIP_DEFAULT_HEARTBEAT_INPUT_ONLY_ASSEMBLY, NULL, 0);
    CreateAssemblyObject(EIP_DEFAULT_HEARTBEAT_LISTEN_ONLY_ASSEMBLY, NULL, 0);

    ConfigureExclusiveOwnerConnectionPoint(0,
                                           g_cfg.consumed_assembly,
                                           g_cfg.produced_assembly,
                                           g_cfg.config_assembly);
    ConfigureInputOnlyConnectionPoint(0,
                                      EIP_DEFAULT_HEARTBEAT_INPUT_ONLY_ASSEMBLY,
                                      g_cfg.produced_assembly,
                                      g_cfg.config_assembly);
    ConfigureListenOnlyConnectionPoint(0,
                                       EIP_DEFAULT_HEARTBEAT_LISTEN_ONLY_ASSEMBLY,
                                       g_cfg.produced_assembly,
                                       g_cfg.config_assembly);

    /* Persist QoS and TCP/IP attribute changes, as a conformant device must. */
    InsertGetSetCallback(GetCipClass(kCipQoSClassCode), NvQosSetCallback,
                         kNvDataFunc);
    InsertGetSetCallback(GetCipClass(kCipTcpIpInterfaceClassCode),
                         NvTcpipSetCallback, kNvDataFunc);
    return kEipStatusOk;
}

void HandleApplication(void) {
    /* Nothing to do: the I/O images move on the IPC thread, not here.  Doing
     * the copy in this call-back would tie the PLC's scan rate to OpENer's
     * select() tick, which is exactly the coupling the ring exists to avoid. */
}

void CheckIoConnectionEvent(unsigned int output_assembly_id,
                            unsigned int input_assembly_id,
                            IoConnectionEvent io_connection_event) {
    (void)output_assembly_id;
    (void)input_assembly_id;

    switch (io_connection_event) {
        case kIoConnectionEventOpened:
            atomic_fetch_add(&g_io_connections, 1);
            break;
        case kIoConnectionEventClosed:
        case kIoConnectionEventTimedOut: {
            uint32_t n = atomic_load(&g_io_connections);
            while (n > 0 &&
                   !atomic_compare_exchange_weak(&g_io_connections, &n, n - 1)) {
                /* retry */
            }
            /* Deliberately do NOT clear g_consumed here.  What the PLC should
             * do when the scanner drops is the core's failsafe policy, decided
             * per adapter; zeroing the image here would silently impose CLEAR
             * on every deployment.  The core stops receiving fresh frames and
             * applies its own choice. */
            break;
        }
        default:
            break;
    }
}

EipStatus AfterAssemblyDataReceived(CipInstance *instance) {
    if (!instance) return kEipStatusError;

    if (instance->instance_number == (CipInstanceNum)g_cfg.consumed_assembly) {
        /* OpENer has just written the scanner's O->T payload into g_consumed.
         * Publish it to the IPC thread now, while we are on OpENer's thread
         * and the buffer is known to be quiescent. */
        pthread_mutex_lock(&g_assembly_lock);
        memcpy(g_consumed_snapshot, g_consumed, sizeof(g_consumed_snapshot));
        pthread_mutex_unlock(&g_assembly_lock);
        atomic_fetch_add(&g_assembly_writes, 1);
        return kEipStatusOk;
    }
    if (instance->instance_number == (CipInstanceNum)g_cfg.config_assembly) {
        /* Accept any configuration assembly.  Rejecting unknown config data
         * fails ODVA conformance tests that send a well-formed but unused
         * config path. */
        return kEipStatusOk;
    }
    return kEipStatusOk;
}

EipBool8 BeforeAssemblyDataSend(CipInstance *instance) {
    /* Called on OpENer's thread immediately before the produced assembly is
     * serialised, which is the one moment we know a transmit is not already in
     * flight.  Latch whatever the IPC thread staged since the last send. */
    if (instance &&
        instance->instance_number == (CipInstanceNum)g_cfg.produced_assembly) {
        pthread_mutex_lock(&g_assembly_lock);
        memcpy(g_produced, g_produced_pending, sizeof(g_produced));
        pthread_mutex_unlock(&g_assembly_lock);
    }
    return true;
}

EipStatus ResetDevice(void) {
    CloseAllConnections();
    CipQosUpdateUsedSetQosValues();
    return kEipStatusOk;
}

EipStatus ResetDeviceToInitialConfiguration(void) {
    g_tcpip.encapsulation_inactivity_timeout = 120;
    CipQosResetAttributesToDefaultValues();
    ResetDevice();
    return kEipStatusOk;
}

void *CipCalloc(size_t number_of_elements, size_t size_of_element) {
    return calloc(number_of_elements, size_of_element);
}

void CipFree(void *data) {
    free(data);
}

void RunIdleChanged(EipUint32 run_idle_value) {
    /* The scanner's run/idle bit.  Reported through the Identity object's
     * extended device status, which is what a conformance tool checks. */
    if ((run_idle_value & 0x0001u) == 1u) {
        CipIdentitySetExtendedDeviceStatus(kAtLeastOneIoConnectionInRunMode);
    } else {
        CipIdentitySetExtendedDeviceStatus(
            kAtLeastOneIoConnectionEstablishedAllInIdleMode);
    }
}

/* --- stack thread -------------------------------------------------------- */

static void *stack_thread(void *arg) {
    (void)arg;
    while (!g_end_stack) {
        if (NetworkHandlerProcessCyclic() != kEipStatusOk) {
            PLC_LOG_ERR("OpENer network handler failed; stopping stack");
            break;
        }
    }
    return NULL;
}

/* --- backend interface --------------------------------------------------- */

static plc_status_t opener_init(const eip_backend_config_t *cfg) {
    if (!cfg || !cfg->interface) return PLC_ERR_INVAL;
    if (cfg->output_bytes > sizeof(g_produced) ||
        cfg->input_bytes  > sizeof(g_consumed)) {
        PLC_LOG_ERR("assembly sizes %u/%u exceed the %zu byte frame limit",
                    cfg->output_bytes, cfg->input_bytes, sizeof(g_produced));
        return PLC_ERR_INVAL;
    }
    g_cfg = *cfg;

    DoublyLinkedListInitialize(&connection_list,
                               CipConnectionObjectListArrayAllocator,
                               CipConnectionObjectListArrayFree);

    /* IfaceGetMacAddress() takes a non-const char*, so give it a copy rather
     * than casting away const on the caller's string. */
    char iface[IFNAMSIZ];
    snprintf(iface, sizeof(iface), "%s", cfg->interface);

    uint8_t mac[6];
    if (IfaceGetMacAddress(iface, mac) == kEipStatusError) {
        PLC_LOG_ERR("network interface '%s' not found", iface);
        return PLC_ERR_IO;
    }

    SetDeviceSerialNumber(123456789);

    /* The upper 16 bits of every CIP connection ID.  ODVA requires this to
     * differ across reboots so a scanner cannot mistake a connection from a
     * previous incarnation for a current one. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srand((unsigned)(ts.tv_nsec ^ ts.tv_sec));
    const EipUint16 unique_connection_id = (EipUint16)rand();

    if (CipStackInit(unique_connection_id) != kEipStatusOk) {
        PLC_LOG_ERR("CipStackInit failed");
        return PLC_ERR_IO;
    }
    g_stack_initialised = 1;

    CipEthernetLinkSetMac(mac);
    GetHostName(&g_tcpip.hostname);

    if (NvdataLoad() == kEipStatusError) {
        PLC_LOG_WARN("no stored NV data; starting from defaults");
    }

    /* Both failure paths below tear the stack down here and then return, and
     * the caller still calls shutdown() afterwards - so clear the flag with
     * each one, or the second ShutdownCipStack() would free the assembly
     * buffers a second time. */
    if (NetworkHandlerInitialize() != kEipStatusOk) {
        PLC_LOG_ERR("NetworkHandlerInitialize failed");
        ShutdownCipStack();
        g_stack_initialised = 0;
        return PLC_ERR_IO;
    }

    g_end_stack = 0;
    if (pthread_create(&g_stack_thread, NULL, stack_thread, NULL) != 0) {
        PLC_LOG_ERR("cannot start OpENer thread: %s", strerror(errno));
        NetworkHandlerFinish();
        ShutdownCipStack();
        g_stack_initialised = 0;
        return PLC_ERR_IO;
    }
    g_stack_running = 1;

    PLC_LOG_INFO("OpENer serving on %s: produced=%u(%uB) consumed=%u(%uB) config=%u",
                 iface,
                 cfg->produced_assembly, cfg->output_bytes,
                 cfg->consumed_assembly, cfg->input_bytes,
                 cfg->config_assembly);
    return PLC_OK;
}

static void opener_shutdown(void) {
    g_end_stack = 1;
    if (g_stack_running) {
        pthread_join(g_stack_thread, NULL);
        g_stack_running = 0;
        NetworkHandlerFinish();
    }
    if (g_stack_initialised) {
        ShutdownCipStack();
        g_stack_initialised = 0;
    }
}

static void opener_publish(const uint8_t *data, size_t len) {
    if (len > sizeof(g_produced_pending)) len = sizeof(g_produced_pending);
    pthread_mutex_lock(&g_assembly_lock);
    if (len) memcpy(g_produced_pending, data, len);
    if (len < sizeof(g_produced_pending)) {
        memset(g_produced_pending + len, 0, sizeof(g_produced_pending) - len);
    }
    pthread_mutex_unlock(&g_assembly_lock);
}

static size_t opener_fetch(uint8_t *data, size_t cap) {
    size_t n = g_cfg.input_bytes;
    if (n > cap) n = cap;
    if (n > sizeof(g_consumed_snapshot)) n = sizeof(g_consumed_snapshot);
    pthread_mutex_lock(&g_assembly_lock);
    if (n) memcpy(data, g_consumed_snapshot, n);
    pthread_mutex_unlock(&g_assembly_lock);
    return n;
}

static uint32_t opener_connections(void) {
    return atomic_load(&g_io_connections);
}

uint64_t eip_opener_assembly_writes(void);

/** Consumed-assembly updates seen since start-up; published as status. */
uint64_t eip_opener_assembly_writes(void) {
    return atomic_load(&g_assembly_writes);
}

static const eip_backend_t kOpenerBackend = {
    .name            = "opener",
    .init            = opener_init,
    .shutdown        = opener_shutdown,
    .publish_outputs = opener_publish,
    .fetch_inputs    = opener_fetch,
    .io_connections  = opener_connections,
};

const eip_backend_t *eip_backend_get(void) { return &kOpenerBackend; }
