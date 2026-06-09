#define _GNU_SOURCE
#include "cpu_affinity.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

int cpu_affinity_pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        fprintf(stderr, "[cpu_affinity] pthread_setaffinity_np cpu=%d rc=%d\n", cpu, rc);
    }

    struct sched_param sp = { .sched_priority = 80 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[cpu_affinity] SCHED_FIFO not available (continuing)\n");
    }
    return rc;
}
