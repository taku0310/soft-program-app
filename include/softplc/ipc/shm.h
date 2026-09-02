/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file shm.h
 * @brief POSIX shared memory and named semaphore helpers.
 *
 * Small on purpose.  Every out-of-process adapter needs the same four things -
 * create/attach a region, open a doorbell, wait on it with a deadline, and
 * clean up - and having one implementation means the Modbus and OPC UA
 * adapters inherit the crash-containment behaviour rather than reinventing it.
 */
#ifndef SOFTPLC_IPC_SHM_H
#define SOFTPLC_IPC_SHM_H

#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>

#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plc_shm {
    char   name[128];
    int    fd;
    void  *base;
    size_t size;
    int    owner;   /**< created it, so responsible for unlinking */
} plc_shm_t;

/** Create (O_EXCL) and map @p size bytes, zeroed.  Replaces a stale region of
 *  the same name: a leftover from a crashed run must not wedge a restart. */
plc_status_t plc_shm_create(plc_shm_t *shm, const char *name, size_t size);

/**
 * @brief Open an existing region and map it.  Fails if it is smaller than
 *        @p size.
 *
 * Does not log, and leaves @c errno set from the failing call: attach is
 * normally polled, so reporting is the caller's to rate-limit.
 * ::PLC_ERR_NOTFOUND means "not published yet" (ENOENT); ::PLC_ERR_IO is
 * anything else, EACCES above all, which will not clear up on its own.
 */
plc_status_t plc_shm_attach(plc_shm_t *shm, const char *name, size_t size);

/** Unmap, close, and unlink when this handle created the region. */
void plc_shm_close(plc_shm_t *shm);

/** Create a named semaphore with value 0, replacing any stale one. */
plc_status_t plc_sem_create(sem_t **sem, const char *name);
/** Open an existing named semaphore.  Silent and errno-preserving, with the
 *  same status split as plc_shm_attach(). */
plc_status_t plc_sem_attach(sem_t **sem, const char *name);
/** Close, and unlink when @p owner is non-zero. */
void plc_sem_close(sem_t *sem, const char *name, int owner);

/**
 * @brief Wait on @p sem for at most @p timeout_us.
 *
 * Uses CLOCK_MONOTONIC where the C library offers sem_clockwait(), so a step
 * of the wall clock cannot stretch or collapse a control cycle's timeout.
 * Retries on EINTR, since a signal must not be reported as a peer timeout.
 *
 * @return ::PLC_OK, ::PLC_ERR_TIMEOUT, or ::PLC_ERR_IO.
 */
plc_status_t plc_sem_wait_timeout(sem_t *sem, uint32_t timeout_us);

/** Non-blocking wait. @return ::PLC_OK or ::PLC_ERR_AGAIN. */
plc_status_t plc_sem_trywait(sem_t *sem);

/** Consume any pending posts without blocking. */
void plc_sem_drain(sem_t *sem);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_IPC_SHM_H */
