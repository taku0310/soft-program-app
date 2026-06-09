#ifndef SOFTPLC_SCHEDULER_CYCLE_MONITOR_H
#define SOFTPLC_SCHEDULER_CYCLE_MONITOR_H

#include <stdint.h>

typedef struct {
    uint64_t target_ns;
    uint64_t count;
    uint64_t sum_ns;
    uint64_t max_ns;
    uint64_t max_jitter_ns;
} cycle_monitor_t;

void cycle_monitor_init(cycle_monitor_t *mon, uint64_t target_ns);
void cycle_monitor_record(cycle_monitor_t *mon, uint64_t cycle_ns);
uint64_t cycle_monitor_avg(const cycle_monitor_t *mon);
uint64_t cycle_monitor_max(const cycle_monitor_t *mon);
uint64_t cycle_monitor_max_jitter(const cycle_monitor_t *mon);
uint64_t cycle_monitor_count(const cycle_monitor_t *mon);

#endif
