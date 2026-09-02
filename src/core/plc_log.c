/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/plc_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static plc_log_level_t g_level = PLC_LOG_LEVEL_INFO;
static char            g_component[32] = "softplc";

static const char *kNames[] = { "ERROR", "WARN", "INFO", "DEBUG" };

void plc_log_set_level(plc_log_level_t level) {
    if (level < PLC_LOG_LEVEL_ERROR) level = PLC_LOG_LEVEL_ERROR;
    if (level > PLC_LOG_LEVEL_DEBUG) level = PLC_LOG_LEVEL_DEBUG;
    g_level = level;
}

plc_log_level_t plc_log_level(void) { return g_level; }

void plc_log_init(const char *component) {
    if (component) {
        snprintf(g_component, sizeof(g_component), "%s", component);
    }
    const char *env = getenv("SOFTPLC_LOG_LEVEL");
    if (!env) return;
    if      (strcmp(env, "error") == 0) g_level = PLC_LOG_LEVEL_ERROR;
    else if (strcmp(env, "warn")  == 0) g_level = PLC_LOG_LEVEL_WARN;
    else if (strcmp(env, "info")  == 0) g_level = PLC_LOG_LEVEL_INFO;
    else if (strcmp(env, "debug") == 0) g_level = PLC_LOG_LEVEL_DEBUG;
}

void plc_log_write(plc_log_level_t level, const char *fmt, ...) {
    if (level > g_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

    flockfile(stderr);
    fprintf(stderr, "%s.%03ldZ %-5s [%s] ",
            stamp, ts.tv_nsec / 1000000, kNames[level], g_component);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}
