#ifndef SOFTPLC_IPC_SHARED_MEMORY_H
#define SOFTPLC_IPC_SHARED_MEMORY_H

#include <stdint.h>

#include "scheduler/cycle_monitor.h"

#define SOFTPLC_SHM_NAME "/softplc_runtime"
#define SOFTPLC_MAX_VARIABLES 1024

typedef struct {
    uint32_t version;
    uint64_t cycle_count;
    uint64_t avg_cycle_ns;
    uint64_t max_cycle_ns;
    uint64_t max_jitter_ns;
    uint32_t variable_count;
    uint8_t variable_blob[SOFTPLC_MAX_VARIABLES];
} softplc_shm_t;

softplc_shm_t *softplc_shm_open_or_create(void);
void softplc_shm_update_diagnostics(softplc_shm_t *shm, const cycle_monitor_t *mon);
void softplc_shm_close(softplc_shm_t *shm);

#endif
