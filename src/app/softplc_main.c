/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file softplc_main.c
 * @brief The PLC core process.
 *
 * Builds a runtime from the environment, asks the registry for whichever
 * protocol adapters were configured, binds them into the process image, loads
 * the demo program and scans.
 *
 * Configuration is environment-driven because the deployment unit is a
 * container: everything here is settable through `docker run -e` or a
 * Kubernetes env block, with no config file to mount.
 *
 *   SOFTPLC_INSTANCE          namespace for IPC objects        (default)
 *   SOFTPLC_ADAPTERS          comma-separated protocols        loopback
 *   SOFTPLC_CYCLE_US          task period in microseconds      10000
 *   SOFTPLC_FAILSAFE          hold | clear                     hold
 *   SOFTPLC_MAX_SCANS         stop after N scans, 0 = forever  0
 *   SOFTPLC_EIP_*             see eip_shm_layout.h
 *   SOFTPLC_LOG_LEVEL         error | warn | info | debug      info
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_program.h"
#include "softplc/adapter_registry.h"
#include "softplc/plc_log.h"
#include "softplc/plc_runtime.h"

#define MAX_ADAPTERS PLC_MAX_BINDINGS

static plc_runtime_t *g_rt;

static void on_signal(int sig) {
    (void)sig;
    /* plc_runtime_request_stop() only writes a sig_atomic_t flag, so it is
     * safe here; the scan finishes and the loop exits cleanly. */
    if (g_rt) plc_runtime_request_stop(g_rt);
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
    return (end == v) ? fallback : (uint32_t)n;
}

static void list_adapters(void) {
    printf("available protocol adapters:\n");
    for (size_t i = 0; i < plc_adapter_registry_count(); ++i) {
        const plc_adapter_factory_t *f = plc_adapter_registry_at(i);
        printf("  %-14s %s\n", f->protocol, f->description);
    }
}

typedef struct opened_adapter {
    const plc_adapter_factory_t *factory;
    plc_protocol_adapter_t      *adapter;
} opened_adapter_t;

int main(int argc, char **argv) {
    plc_log_init("softplc");
    plc_adapter_register_builtins();

    if (argc > 1 && strcmp(argv[1], "--list-adapters") == 0) {
        list_adapters();
        return EXIT_SUCCESS;
    }

    const char *instance = env_str("SOFTPLC_INSTANCE", "default");
    const char *failsafe = env_str("SOFTPLC_FAILSAFE", "hold");
    const plc_failsafe_policy_t policy =
        (strcmp(failsafe, "clear") == 0) ? PLC_FAILSAFE_CLEAR : PLC_FAILSAFE_HOLD;

    plc_runtime_config_t rc;
    plc_runtime_config_init(&rc);
    rc.cycle_us = env_u32("SOFTPLC_CYCLE_US", rc.cycle_us);

    g_rt = plc_runtime_create(&rc);
    if (!g_rt) {
        PLC_LOG_ERR("cannot create runtime");
        return EXIT_FAILURE;
    }

    /* --- adapters ------------------------------------------------------- */

    opened_adapter_t opened[MAX_ADAPTERS];
    size_t opened_count = 0;
    size_t i_offset = 0, q_offset = 0;

    char list[256];
    snprintf(list, sizeof(list), "%s", env_str("SOFTPLC_ADAPTERS", "loopback"));

    for (char *save = NULL, *tok = strtok_r(list, ",", &save);
         tok && opened_count < MAX_ADAPTERS;
         tok = strtok_r(NULL, ",", &save)) {

        while (*tok == ' ') tok++;
        if (!*tok) continue;

        const plc_adapter_factory_t *f = plc_adapter_registry_find(tok);
        if (!f) {
            PLC_LOG_ERR("no adapter for protocol '%s'", tok);
            list_adapters();
            goto fail;
        }

        plc_protocol_adapter_t *a = f->create();
        if (!a) { PLC_LOG_ERR("cannot allocate '%s' adapter", tok); goto fail; }

        plc_adapter_config_t ac;
        plc_adapter_config_init(&ac);
        ac.name     = tok;
        ac.endpoint = instance;
        ac.failsafe_policy = policy;

        plc_status_t st = plc_adapter_open(a, &ac);
        if (st != PLC_OK) {
            PLC_LOG_ERR("opening '%s' failed: %s", tok, plc_strerror(st));
            f->destroy(a);
            goto fail;
        }

        opened[opened_count].factory = f;
        opened[opened_count].adapter = a;
        opened_count++;

        /* Adapters are laid out back to back in %I and %Q in the order they
         * were configured, so the map is reproducible from SOFTPLC_ADAPTERS
         * alone and does not need a separate mapping file. */
        st = plc_runtime_bind_adapter(g_rt, a, i_offset, q_offset);
        if (st != PLC_OK) {
            PLC_LOG_ERR("binding '%s' failed: %s", tok, plc_strerror(st));
            goto fail;
        }

        plc_adapter_caps_t caps;
        plc_adapter_get_caps(a, &caps);
        i_offset += caps.input_bytes;
        q_offset += caps.output_bytes;
    }

    /* --- program -------------------------------------------------------- */

    static demo_state_t demo;
    if (plc_runtime_add_program(g_rt, "demo", demo_program, &demo) != PLC_OK) {
        PLC_LOG_ERR("cannot register the demo program");
        goto fail;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    PLC_LOG_INFO("instance '%s': %zu adapter(s), %u us cycle, failsafe=%s",
                 instance, opened_count, rc.cycle_us,
                 policy == PLC_FAILSAFE_CLEAR ? "CLEAR" : "HOLD");

    const uint64_t max_scans = env_u32("SOFTPLC_MAX_SCANS", 0);
    const plc_status_t run_st = plc_runtime_run(g_rt, max_scans);

    plc_runtime_stats_t rs;
    plc_runtime_get_stats(g_rt, &rs);
    PLC_LOG_INFO("stopped after %llu scans (overruns=%llu io_faults=%llu "
                 "max_scan=%lluus max_jitter=%lluus)",
                 (unsigned long long)rs.scans,
                 (unsigned long long)rs.overruns,
                 (unsigned long long)rs.io_faults,
                 (unsigned long long)rs.max_scan_us,
                 (unsigned long long)rs.max_jitter_us);

    for (size_t i = 0; i < opened_count; ++i) {
        plc_adapter_stats_t as;
        plc_adapter_caps_t  ac;
        plc_adapter_get_stats(opened[i].adapter, &as);
        plc_adapter_get_caps(opened[i].adapter, &ac);
        PLC_LOG_INFO("adapter '%s' [%s]: %llu exchanges, %llu timeouts, "
                     "%llu failsafe activations, max_rtt=%lluus",
                     ac.name, plc_adapter_state_name(plc_adapter_state(opened[i].adapter)),
                     (unsigned long long)as.exchanges,
                     (unsigned long long)as.timeouts,
                     (unsigned long long)as.failsafe_activations,
                     (unsigned long long)as.max_rtt_us);
        opened[i].factory->destroy(opened[i].adapter);
    }
    plc_runtime_destroy(g_rt);
    return (run_st == PLC_OK) ? EXIT_SUCCESS : EXIT_FAILURE;

fail:
    for (size_t i = 0; i < opened_count; ++i) {
        opened[i].factory->destroy(opened[i].adapter);
    }
    plc_runtime_destroy(g_rt);
    return EXIT_FAILURE;
}
