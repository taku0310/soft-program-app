/* SPDX-License-Identifier: Apache-2.0 */
#include "eip_shm_layout.h"

#include <stdio.h>
#include <string.h>

/* Shared by both processes so a typo cannot make them disagree about which
 * objects to open - a mismatch here would look exactly like a dead peer. */
static void build(char *buf, size_t buf_len, const char *instance,
                  const char *suffix) {
    if (!buf || buf_len == 0) return;
    if (!instance || !*instance) instance = "default";
    snprintf(buf, buf_len, "/softplc.%s.eip%s", instance, suffix);
}

void eip_shm_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, "");
}

void eip_sem_req_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, ".req");
}

void eip_sem_rsp_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, ".rsp");
}
