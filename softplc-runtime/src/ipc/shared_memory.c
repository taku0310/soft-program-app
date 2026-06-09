#define _POSIX_C_SOURCE 200809L

#include "shared_memory.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

softplc_shm_t *softplc_shm_open_or_create(void) {
    int fd = shm_open(SOFTPLC_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof(softplc_shm_t)) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, sizeof(softplc_shm_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    softplc_shm_t *shm = p;
    if (shm->version == 0) {
        memset(shm, 0, sizeof(*shm));
        shm->version = 1;
    }
    return shm;
}

void softplc_shm_update_diagnostics(softplc_shm_t *shm, const cycle_monitor_t *mon) {
    if (!shm || !mon) return;
    shm->cycle_count = cycle_monitor_count(mon);
    shm->avg_cycle_ns = cycle_monitor_avg(mon);
    shm->max_cycle_ns = cycle_monitor_max(mon);
    shm->max_jitter_ns = cycle_monitor_max_jitter(mon);
}

void softplc_shm_close(softplc_shm_t *shm) {
    if (!shm) return;
    munmap(shm, sizeof(*shm));
}
