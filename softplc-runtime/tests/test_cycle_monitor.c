#include <assert.h>
#include <stdio.h>

#include "scheduler/cycle_monitor.h"

int main(void) {
    cycle_monitor_t mon;
    cycle_monitor_init(&mon, 10000);

    cycle_monitor_record(&mon, 9800);
    cycle_monitor_record(&mon, 10100);
    cycle_monitor_record(&mon, 10500);

    assert(cycle_monitor_count(&mon) == 3);
    assert(cycle_monitor_max(&mon) == 10500);

    printf("cycle_monitor tests passed\n");
    return 0;
}
