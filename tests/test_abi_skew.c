/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Core and stack are separate binaries in separate containers, so a rolling
 * update runs them at different versions for a while. The shared region
 * carries an ABI version and its own size for exactly that window, and both
 * attach paths check them - but nothing had ever exercised the check, and an
 * unexercised refusal is indistinguishable from no refusal at all until the
 * day it matters.
 *
 * What must not happen is the stack attaching anyway and reading the fields
 * at the offsets it expects: the process image is the output of a machine,
 * and a shifted one is worse than none.
 *
 * The region is built here by hand rather than by running a second core, so
 * the mismatch is exact and the test needs no second build of the tree.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/adapters/eip/eip_shm_layout.h"
#include "test_util.h"

/** Publish a region that looks ready but declares @p abi and @p bytes. */
static void publish(const char *name, uint32_t abi, uint32_t bytes, uint32_t magic) {
    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    CHECK(fd >= 0);
    CHECK_EQ_INT(ftruncate(fd, (off_t)sizeof(eip_shm_t)), 0);
    eip_shm_t *m = mmap(NULL, sizeof(eip_shm_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    close(fd);

    memset(m, 0, sizeof(*m));
    m->output_bytes = 32;
    m->input_bytes  = 32;
    plc_spsc_init(&m->req);
    plc_spsc_init(&m->rsp);
    m->abi_version  = abi;
    m->layout_bytes = bytes;
    /* Magic last, as the core does: it is the "everything is published" flag. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->magic = magic;
    munmap(m, sizeof(eip_shm_t));
}

/** Run the real stack against it; returns its exit code, -1 on a timeout. */
static int run_stack(const char *instance) {
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* Its complaint belongs in the test log, not in the suite's output. */
        if (!freopen("/dev/null", "w", stderr)) { /* its log is not the test result */ }
        execl(EIP_ADAPTER_PATH, "softplc-eip-adapter", instance, "lo", (char *)NULL);
        _exit(127);
    }

    /* It must refuse promptly. Attach retries on "not published yet", so a
     * mismatch that is treated as "keep waiting" would hang here - which is
     * itself a failure: a rolling update must not leave a stack spinning
     * against a peer it can never talk to without saying why. */
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void check_refused(const char *what, uint32_t abi, uint32_t bytes,
                          uint32_t magic) {
    char instance[64], name[EIP_SHM_NAME_MAX];
    snprintf(instance, sizeof(instance), "abiskew-%d-%s", (int)getpid(), what);
    eip_shm_name(name, sizeof(name), instance);

    publish(name, abi, bytes, magic);
    const int rc = run_stack(instance);
    if (rc == -1) {
        fprintf(stderr, "FAIL %s: the stack never gave up on a %s region\n",
                __FILE__, what);
        CHECK(0);
    } else {
        /* Non-zero, and it exited on its own. */
        CHECK(rc != 0);
    }
    shm_unlink(name);
}

int main(void) {
    /* One version apart, the rolling-update case. */
    check_refused("abi", EIP_SHM_ABI_VERSION + 1u,
                  (uint32_t)sizeof(eip_shm_t), EIP_SHM_MAGIC);
    /* Same version, different layout: a field added without a version bump,
     * which is the mistake the size check exists to catch. */
    check_refused("layout", EIP_SHM_ABI_VERSION,
                  (uint32_t)sizeof(eip_shm_t) + 8u, EIP_SHM_MAGIC);
    /* Not our region at all. */
    check_refused("magic", EIP_SHM_ABI_VERSION,
                  (uint32_t)sizeof(eip_shm_t), 0xDEADBEEFu);

    TEST_REPORT("abi_skew");
}
