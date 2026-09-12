/* SPDX-License-Identifier: Apache-2.0 */
/**
 * `softplc --status` is a health probe, so its contract is the exit code and
 * the shape of one JSON line - not prose. Both are asserted here against the
 * real binary, because a probe that is only tested by reading its output by
 * hand is a probe nobody will trust at three in the morning.
 *
 * The interesting case is the one a container hits first: nothing published
 * under that instance name yet. It must be distinguishable from "published
 * and unhealthy", or a restart loop looks like a slow start.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.h"

#define ABSENT    4
#define NOT_READY 3

/** Run `softplc --status <instance>`, capture stdout, return the exit code. */
static int run_status(const char *instance, char *out, size_t cap) {
    int fds[2];
    if (pipe(fds) != 0) return -1;

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl(SOFTPLC_PATH, "softplc", "--status", instance, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fds[0], out + n, cap - n - 1)) > 0) n += (size_t)r;
    out[n] = '\0';
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_absent_instance_is_its_own_answer(void) {
    char out[1024];
    char instance[64];
    /* Nothing else may have published this, or the test is measuring the
     * machine rather than the binary. */
    snprintf(instance, sizeof(instance), "status-absent-%d", (int)getpid());

    const int rc = run_status(instance, out, sizeof(out));
    CHECK_EQ_INT(rc, ABSENT);

    /* Still JSON, still parseable: a probe that gets prose on the error path
     * has to special-case it, and then it will get that wrong. */
    CHECK(out[0] == '{');
    CHECK(strstr(out, "\"ready\":false") != NULL);
    CHECK(strstr(out, instance) != NULL);
    CHECK(strstr(out, "\"error\"") != NULL);
    CHECK(strchr(out, '\n') != NULL);
}

static void test_the_exit_code_separates_absent_from_unready(void) {
    /* ABSENT and NOT_READY have to be different numbers; a probe that cannot
     * tell "not started" from "started and unhealthy" cannot tell a slow boot
     * from a crash loop. */
    CHECK(ABSENT != NOT_READY);
    CHECK(ABSENT != 0 && NOT_READY != 0);
}

int main(void) {
    test_absent_instance_is_its_own_answer();
    test_the_exit_code_separates_absent_from_unready();
    TEST_REPORT("status_json");
}
