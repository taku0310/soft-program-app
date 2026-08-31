/* SPDX-License-Identifier: Apache-2.0 */
#include "eip_scanner_shm_layout.h"

#include <stdio.h>

/* Distinct from the adapter's "/softplc.<instance>.eip" so that both roles can
 * run against the same instance name without colliding - which they do, since
 * this deployment keeps the adapter role as well. */
static void build(char *buf, size_t buf_len, const char *instance,
                  const char *suffix) {
    if (!buf || buf_len == 0) return;
    if (!instance || !*instance) instance = "default";
    snprintf(buf, buf_len, "/softplc.%s.eipscan%s", instance, suffix);
}

void eip_scanner_shm_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, "");
}
void eip_scanner_sem_req_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, ".req");
}
void eip_scanner_sem_rsp_name(char *buf, size_t buf_len, const char *instance) {
    build(buf, buf_len, instance, ".rsp");
}
