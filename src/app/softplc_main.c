/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file softplc_main.c
 * @brief The PLC core process.
 *
 * Builds a runtime from its configuration, asks the registry for whichever
 * protocol adapters that names, binds them into the process image, loads the
 * demo program and scans.
 *
 * Settings come from the environment first and a config file underneath it
 * (see plc_config.h); everything below is settable either way, so a container
 * can be driven entirely by `docker run -e` or entirely by a mounted
 * `softplc.conf`.
 *
 *   SOFTPLC_ROLE              none | adapter | scanner         none
 *   SOFTPLC_INSTANCE          namespace for IPC objects        (default)
 *   SOFTPLC_ADAPTERS          comma-separated protocols        (from the role)
 *   SOFTPLC_CYCLE_US          task period in microseconds      10000
 *   SOFTPLC_FAILSAFE          hold | clear                     hold
 *   SOFTPLC_MAX_SCANS         stop after N scans, 0 = forever  0
 *   SOFTPLC_EIP_*             see eip_shm_layout.h
 *   SOFTPLC_LOG_LEVEL         error | warn | info | debug      info
 *
 * `role` is the setting a single-container deployment turns: it picks the
 * adapter the core binds *and*, read back through `--role`, tells the
 * entrypoint which stack process to run beside it.  Both come from one value
 * so the two halves cannot be configured to disagree.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_program.h"
#include "softplc/adapter_registry.h"
#include "softplc/plc_config.h"
#include "softplc/plc_log.h"
#include "softplc/plc_runtime.h"
#include "softplc_status.h"

#define MAX_ADAPTERS PLC_MAX_BINDINGS

static plc_runtime_t *g_rt;

static void on_signal(int sig) {
    (void)sig;
    /* plc_runtime_request_stop() only writes a sig_atomic_t flag, so it is
     * safe here; the scan finishes and the loop exits cleanly. */
    if (g_rt) plc_runtime_request_stop(g_rt);
}

static void list_adapters(void) {
    printf("available protocol adapters:\n");
    for (size_t i = 0; i < plc_adapter_registry_count(); ++i) {
        const plc_adapter_factory_t *f = plc_adapter_registry_at(i);
        printf("  %-14s %s\n", f->protocol, f->description);
    }
}

static void list_roles(void) {
    printf("available roles:\n");
    for (size_t i = 0; ; ++i) {
        const char *role = plc_adapter_role_at(i);
        if (!role) break;
        printf("  %-10s %s\n", role, plc_adapter_role_adapters(role));
    }
}

/**
 * Dump what the file said and what each setting actually resolved to.
 *
 * The question this answers is the one that costs an afternoon otherwise:
 * "did my config file take effect, or is something in the environment still
 * winning?"  So every entry says where its value came from.
 */
static void show_config(void) {
    if (plc_config_count()) {
        printf("config file: %s\n", plc_config_path());
        for (size_t i = 0; i < plc_config_count(); ++i) {
            const char *key = NULL, *value = NULL;
            plc_config_entry_at(i, &key, &value);
            const char *effective = plc_cfg_str(key, value);
            printf("  %-32s = %s%s\n", key, value,
                   (strcmp(effective, value) == 0)
                       ? "" : "   (overridden by the environment)");
        }
    } else {
        printf("config file: none (%s absent and SOFTPLC_CONFIG unset)\n",
               PLC_CONFIG_DEFAULT_PATH);
    }

    const char *role = plc_cfg_str("SOFTPLC_ROLE", "none");
    const char *from_role = plc_adapter_role_adapters(role);
    printf("resolved:\n");
    printf("  %-32s = %s%s\n", "SOFTPLC_ROLE", role,
           from_role ? "" : "   *** unknown role ***");
    printf("  %-32s = %s\n", "SOFTPLC_ADAPTERS",
           plc_cfg_str("SOFTPLC_ADAPTERS", from_role ? from_role : "?"));
    printf("  %-32s = %s\n", "SOFTPLC_INSTANCE",
           plc_cfg_str("SOFTPLC_INSTANCE", "default"));
    printf("  %-32s = %u\n", "SOFTPLC_CYCLE_US",
           plc_cfg_u32("SOFTPLC_CYCLE_US", 10000));
    printf("  %-32s = %s\n", "SOFTPLC_FAILSAFE",
           plc_cfg_str("SOFTPLC_FAILSAFE", "hold"));
}

typedef struct opened_adapter {
    const plc_adapter_factory_t *factory;
    plc_protocol_adapter_t      *adapter;
} opened_adapter_t;

int main(int argc, char **argv) {
    if (plc_config_bootstrap("softplc") != 0) return EXIT_FAILURE;
    plc_adapter_register_builtins();

    /* --- resolve the role ----------------------------------------------- */

    const char *role = plc_cfg_str("SOFTPLC_ROLE", "none");
    const char *role_adapters = plc_adapter_role_adapters(role);

    if (argc > 1) {
        if (strcmp(argv[1], "--list-adapters") == 0) {
            list_adapters();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--list-roles") == 0) {
            list_roles();
            return EXIT_SUCCESS;
        }
        /* Health as a value rather than as prose. Kept here rather than in a
         * separate binary so a container that carries `softplc` carries its
         * own probe: `softplc --status` and read the exit code. */
        if (strcmp(argv[1], "--status") == 0) {
            const char *inst = (argc > 2) ? argv[2]
                                          : plc_cfg_str("SOFTPLC_INSTANCE", "default");
            return softplc_print_status(inst);
        }
        if (strcmp(argv[1], "--show-config") == 0) {
            show_config();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--print-config") == 0) {
            if (argc < 3) {
                fprintf(stderr, "usage: softplc --print-config KEY\n");
                return EXIT_FAILURE;
            }
            printf("%s\n", plc_cfg_str(argv[2], ""));
            return EXIT_SUCCESS;
        }
        /* The entrypoint's single source of truth for which stack process to
         * start.  It validates here, in the binary that owns the role table,
         * so an unknown role fails before anything is launched rather than
         * leaving a core scanning next to no stack at all. */
        if (strcmp(argv[1], "--role") == 0) {
            if (!role_adapters) {
                fprintf(stderr, "unknown role '%s'\n", role);
                list_roles();
                return EXIT_FAILURE;
            }
            printf("%s\n", role);
            return EXIT_SUCCESS;
        }
    }

    if (!role_adapters) {
        PLC_LOG_ERR("unknown role '%s'", role);
        list_roles();
        return EXIT_FAILURE;
    }

    const char *instance = plc_cfg_str("SOFTPLC_INSTANCE", "default");
    const char *failsafe = plc_cfg_str("SOFTPLC_FAILSAFE", "hold");
    const plc_failsafe_policy_t policy =
        (strcmp(failsafe, "clear") == 0) ? PLC_FAILSAFE_CLEAR : PLC_FAILSAFE_HOLD;

    plc_runtime_config_t rc;
    plc_runtime_config_init(&rc);
    rc.cycle_us = plc_cfg_u32("SOFTPLC_CYCLE_US", rc.cycle_us);

    g_rt = plc_runtime_create(&rc);
    if (!g_rt) {
        PLC_LOG_ERR("cannot create runtime");
        return EXIT_FAILURE;
    }

    /* --- adapters ------------------------------------------------------- */

    opened_adapter_t opened[MAX_ADAPTERS];
    size_t opened_count = 0;
    size_t i_offset = 0, q_offset = 0;

    /* The role already names an adapter list.  An explicit SOFTPLC_ADAPTERS
     * still overrides it, because a deployment that genuinely wants two
     * protocols bound at once has no other way to say so - but when both are
     * set and they disagree, say so: the role still decides which stack
     * process runs beside this one, and a silent mismatch there is a core
     * scanning against a peer that is not there. */
    const char *explicit_list = plc_cfg_str("SOFTPLC_ADAPTERS", NULL);
    if (explicit_list && plc_cfg_str("SOFTPLC_ROLE", NULL) &&
        strcmp(explicit_list, role_adapters) != 0) {
        PLC_LOG_WARN("role '%s' implies adapters '%s' but SOFTPLC_ADAPTERS is "
                     "'%s'; the list wins here, the role still picks the stack "
                     "process", role, role_adapters, explicit_list);
    }

    char list[256];
    snprintf(list, sizeof(list), "%s", explicit_list ? explicit_list : role_adapters);

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

    PLC_LOG_INFO("instance '%s': role=%s, %zu adapter(s), %u us cycle, "
                 "failsafe=%s, config from %s",
                 instance, role, opened_count, rc.cycle_us,
                 policy == PLC_FAILSAFE_CLEAR ? "CLEAR" : "HOLD",
                 plc_config_source());

    const uint64_t max_scans = plc_cfg_u32("SOFTPLC_MAX_SCANS", 0);
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
