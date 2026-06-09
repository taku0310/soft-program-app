#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ethernet_ip/connection_manager.h"
#include "ipc/shared_memory.h"
#include "mqtt/mqtt_publisher.h"
#include "scheduler/cpu_affinity.h"
#include "scheduler/cycle_monitor.h"

#define CYCLE_NS (10ULL * 1000ULL * 1000ULL)

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void timespec_add_ns(struct timespec *ts, unsigned long long ns) {
    ts->tv_nsec += (long)(ns % 1000000000ULL);
    ts->tv_sec += (time_t)(ns / 1000000000ULL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

static unsigned long long elapsed_ns(const struct timespec *start, const struct timespec *end) {
    return (unsigned long long)(end->tv_sec - start->tv_sec) * 1000000000ULL
         + (unsigned long long)(end->tv_nsec - start->tv_nsec);
}

int main(int argc, char *argv[]) {
    const char *config_path = "/etc/softplc/plc_config.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0) config_path = argv[i + 1];
    }
    fprintf(stderr, "[plc_runtime] starting with config=%s\n", config_path);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[plc_runtime] mlockall failed: %s (continuing)\n", strerror(errno));
    }
    cpu_affinity_pin_self(1);

    softplc_shm_t *shm = softplc_shm_open_or_create();
    if (!shm) {
        fprintf(stderr, "[plc_runtime] shm_open failed\n");
        return 1;
    }

    if (cip_connection_manager_init(config_path) != 0) {
        fprintf(stderr, "[plc_runtime] CIP init failed\n");
        return 1;
    }
    if (mqtt_publisher_init(config_path) != 0) {
        fprintf(stderr, "[plc_runtime] MQTT init failed\n");
        return 1;
    }

    cycle_monitor_t cycle_mon;
    cycle_monitor_init(&cycle_mon, CYCLE_NS);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    unsigned long long mqtt_accumulator_ns = 0;

    while (g_running) {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);

        cip_connection_manager_sample_inputs();
        /* TODO: execute compiled IEC 61131-3 logic */
        cip_connection_manager_sync_outputs();

        mqtt_accumulator_ns += CYCLE_NS;
        if (mqtt_accumulator_ns >= 1000000000ULL) {
            mqtt_publisher_tick();
            mqtt_accumulator_ns = 0;
        }

        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        cycle_monitor_record(&cycle_mon, elapsed_ns(&start, &end));
        softplc_shm_update_diagnostics(shm, &cycle_mon);

        timespec_add_ns(&next, CYCLE_NS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    mqtt_publisher_shutdown();
    cip_connection_manager_shutdown();
    softplc_shm_close(shm);
    fprintf(stderr, "[plc_runtime] stopped\n");
    return 0;
}
