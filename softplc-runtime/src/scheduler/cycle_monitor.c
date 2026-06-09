#include "cycle_monitor.h"

#include <string.h>

void cycle_monitor_init(cycle_monitor_t *mon, uint64_t target_ns) {
    memset(mon, 0, sizeof(*mon));
    mon->target_ns = target_ns;
}

void cycle_monitor_record(cycle_monitor_t *mon, uint64_t cycle_ns) {
    mon->count += 1;
    mon->sum_ns += cycle_ns;
    if (cycle_ns > mon->max_ns) mon->max_ns = cycle_ns;

    uint64_t jitter = cycle_ns > mon->target_ns ? cycle_ns - mon->target_ns
                                                : mon->target_ns - cycle_ns;
    if (jitter > mon->max_jitter_ns) mon->max_jitter_ns = jitter;
}

uint64_t cycle_monitor_avg(const cycle_monitor_t *mon) {
    return mon->count ? mon->sum_ns / mon->count : 0;
}

uint64_t cycle_monitor_max(const cycle_monitor_t *mon) { return mon->max_ns; }
uint64_t cycle_monitor_max_jitter(const cycle_monitor_t *mon) { return mon->max_jitter_ns; }
uint64_t cycle_monitor_count(const cycle_monitor_t *mon) { return mon->count; }
