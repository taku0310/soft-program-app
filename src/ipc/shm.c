/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "softplc/ipc/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "softplc/plc_log.h"

plc_status_t plc_shm_create(plc_shm_t *shm, const char *name, size_t size) {
    if (!shm || !name || size == 0) return PLC_ERR_INVAL;
    memset(shm, 0, sizeof(*shm));
    snprintf(shm->name, sizeof(shm->name), "%s", name);

    /* A region left behind by a crashed run would otherwise be inherited with
     * whatever cursors it died holding.  Unlink first so we always start from
     * a known-empty ring; anyone still attached keeps their mapping and simply
     * stops being visible to us, which is the containment behaviour we want. */
    shm_unlink(name);

    shm->fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm->fd < 0) {
        PLC_LOG_ERR("shm_open(%s) create failed: %s", name, strerror(errno));
        return PLC_ERR_IO;
    }
    if (ftruncate(shm->fd, (off_t)size) != 0) {
        PLC_LOG_ERR("ftruncate(%s, %zu) failed: %s", name, size, strerror(errno));
        close(shm->fd);
        shm->fd = -1;   /* the handle must not keep a descriptor it has closed */
        shm_unlink(name);
        return PLC_ERR_IO;
    }

    shm->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->base == MAP_FAILED) {
        PLC_LOG_ERR("mmap(%s) failed: %s", name, strerror(errno));
        shm->base = NULL;
        close(shm->fd);
        shm->fd = -1;
        shm_unlink(name);
        return PLC_ERR_NOMEM;
    }

    memset(shm->base, 0, size);
    shm->size  = size;
    shm->owner = 1;
    return PLC_OK;
}

plc_status_t plc_shm_attach(plc_shm_t *shm, const char *name, size_t size) {
    if (!shm || !name || size == 0) return PLC_ERR_INVAL;
    memset(shm, 0, sizeof(*shm));
    snprintf(shm->name, sizeof(shm->name), "%s", name);

    /* Attach is polled in a retry loop, so this must not log: at 200 ms a
     * tick, a permanent failure such as EACCES would flood the log for the
     * life of the container.  Distinguish the benign case in the status and
     * leave errno intact for the caller to report once - nothing below the
     * return touches it. */
    shm->fd = shm_open(name, O_RDWR, 0);
    if (shm->fd < 0) {
        const int err = errno;
        shm->fd = -1;
        errno = err;
        return (err == ENOENT) ? PLC_ERR_NOTFOUND : PLC_ERR_IO;
    }

    /* Silent for the same reason as the open above; PLC_ERR_PROTO is its own
     * status so the caller can say what is wrong without consulting errno,
     * which this path does not set. */
    struct stat st;
    if (fstat(shm->fd, &st) != 0 || (size_t)st.st_size < size) {
        close(shm->fd);
        shm->fd = -1;
        return PLC_ERR_PROTO;
    }

    shm->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->base == MAP_FAILED) {
        shm->base = NULL;
        close(shm->fd);
        shm->fd = -1;
        return PLC_ERR_NOMEM;
    }
    shm->size  = size;
    shm->owner = 0;
    return PLC_OK;
}

void plc_shm_close(plc_shm_t *shm) {
    if (!shm) return;
    if (shm->base) munmap(shm->base, shm->size);
    if (shm->fd >= 0) close(shm->fd);
    if (shm->owner && shm->name[0]) shm_unlink(shm->name);
    memset(shm, 0, sizeof(*shm));
    shm->fd = -1;
}

plc_status_t plc_sem_create(sem_t **sem, const char *name) {
    if (!sem || !name) return PLC_ERR_INVAL;
    sem_unlink(name);   /* same stale-object reasoning as plc_shm_create */
    *sem = sem_open(name, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0);
    if (*sem == SEM_FAILED) {
        PLC_LOG_ERR("sem_open(%s) create failed: %s", name, strerror(errno));
        *sem = NULL;
        return PLC_ERR_IO;
    }
    return PLC_OK;
}

plc_status_t plc_sem_attach(sem_t **sem, const char *name) {
    if (!sem || !name) return PLC_ERR_INVAL;
    /* Silent and errno-preserving, for the same reason as plc_shm_attach(). */
    *sem = sem_open(name, 0);
    if (*sem == SEM_FAILED) {
        const int err = errno;
        *sem = NULL;
        errno = err;
        return (err == ENOENT) ? PLC_ERR_NOTFOUND : PLC_ERR_IO;
    }
    return PLC_OK;
}

void plc_sem_close(sem_t *sem, const char *name, int owner) {
    if (sem) sem_close(sem);
    if (owner && name) sem_unlink(name);
}

plc_status_t plc_sem_wait_timeout(sem_t *sem, uint32_t timeout_us) {
    if (!sem) return PLC_ERR_INVAL;
    if (timeout_us == 0) return plc_sem_trywait(sem);

#if defined(__GLIBC__) && defined(_POSIX_TIMERS)
#  define SOFTPLC_HAVE_SEM_CLOCKWAIT (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
#else
#  define SOFTPLC_HAVE_SEM_CLOCKWAIT 0
#endif

#if SOFTPLC_HAVE_SEM_CLOCKWAIT
    const clockid_t clk = CLOCK_MONOTONIC;
#else
    /* Fallback: POSIX only guarantees CLOCK_REALTIME for sem_timedwait, which
     * means a wall-clock step can distort this deadline.  Acceptable only
     * because the failsafe path treats a spurious timeout as a held frame. */
    const clockid_t clk = CLOCK_REALTIME;
#endif

    struct timespec deadline;
    clock_gettime(clk, &deadline);
    deadline.tv_nsec += (long)(timeout_us % 1000000u) * 1000L;
    deadline.tv_sec  += (time_t)(timeout_us / 1000000u);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec  += 1;
    }

    for (;;) {
#if SOFTPLC_HAVE_SEM_CLOCKWAIT
        const int rc = sem_clockwait(sem, clk, &deadline);
#else
        const int rc = sem_timedwait(sem, &deadline);
#endif
        if (rc == 0) return PLC_OK;
        if (errno == EINTR) continue;   /* a signal is not a peer timeout */
        if (errno == ETIMEDOUT) return PLC_ERR_TIMEOUT;
        return PLC_ERR_IO;
    }
}

plc_status_t plc_sem_trywait(sem_t *sem) {
    if (!sem) return PLC_ERR_INVAL;
    for (;;) {
        if (sem_trywait(sem) == 0) return PLC_OK;
        if (errno == EINTR) continue;
        return (errno == EAGAIN) ? PLC_ERR_AGAIN : PLC_ERR_IO;
    }
}

void plc_sem_drain(sem_t *sem) {
    if (!sem) return;
    while (plc_sem_trywait(sem) == PLC_OK) { /* discard */ }
}
