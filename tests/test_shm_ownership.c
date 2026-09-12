/* SPDX-License-Identifier: Apache-2.0 */
/**
 * A shared-memory name belongs to whoever is still running under it.
 *
 * Creating a region unlinks any region of the same name first, so that a
 * crashed run cannot wedge its own restart. Read literally that also let a
 * second core on the same instance name replace a live one's region without a
 * word: the incumbent kept scanning against a mapping nothing would answer,
 * and which core the stack served came down to restart order. Both halves are
 * asserted here, because fixing the second by dropping the unlink would
 * reintroduce the first.
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "softplc/ipc/shm.h"
#include "test_util.h"

#define NAME "/softplc.test.ownership"
#define SIZE 4096u

static void test_a_live_owner_is_not_evicted(void) {
    plc_shm_t first;
    CHECK_EQ_INT(plc_shm_create(&first, NAME, SIZE), PLC_OK);

    /* Same process or another makes no difference to the lock; the point is
     * that someone holds it. */
    plc_shm_t second;
    CHECK_EQ_INT(plc_shm_create(&second, NAME, SIZE), PLC_ERR_STATE);

    /* Refused means refused all the way: the incumbent's region is still
     * mapped, still its own, and still named. */
    *(volatile uint32_t *)first.base = 0xA5A5A5A5u;
    CHECK_EQ_INT(*(volatile uint32_t *)first.base, 0xA5A5A5A5u);

    plc_shm_t attached;
    CHECK_EQ_INT(plc_shm_attach(&attached, NAME, SIZE), PLC_OK);
    CHECK_EQ_INT(*(volatile uint32_t *)attached.base, 0xA5A5A5A5u);
    plc_shm_close(&attached);

    plc_shm_close(&first);
}

static void test_a_dead_owner_leaves_nothing_behind(void) {
    /* A child that dies holding the region, the way a SIGKILLed core does:
     * no cleanup runs, so the name and its contents outlive it. */
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        plc_shm_t child;
        if (plc_shm_create(&child, NAME, SIZE) != PLC_OK) _exit(2);
        *(volatile uint32_t *)child.base = 0xDEADBEEFu;
        pause();
        _exit(0);
    }
    /* Give the child time to create it, then kill it outright. */
    usleep(200000);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);

    /* The region is still there... */
    const int fd = shm_open(NAME, O_RDWR, 0);
    CHECK(fd >= 0);
    close(fd);

    /* ...and must not stop the restart, nor hand it the dead run's cursors. */
    plc_shm_t restarted;
    CHECK_EQ_INT(plc_shm_create(&restarted, NAME, SIZE), PLC_OK);
    CHECK_EQ_INT(*(volatile uint32_t *)restarted.base, 0u);
    plc_shm_close(&restarted);
}

static void test_the_name_is_free_once_the_owner_closes(void) {
    plc_shm_t first;
    CHECK_EQ_INT(plc_shm_create(&first, NAME, SIZE), PLC_OK);
    plc_shm_close(&first);

    plc_shm_t second;
    CHECK_EQ_INT(plc_shm_create(&second, NAME, SIZE), PLC_OK);
    plc_shm_close(&second);
}

int main(void) {
    shm_unlink(NAME);
    test_a_live_owner_is_not_evicted();
    test_a_dead_owner_leaves_nothing_behind();
    test_the_name_is_free_once_the_owner_closes();
    shm_unlink(NAME);
    TEST_REPORT("shm_ownership");
}
