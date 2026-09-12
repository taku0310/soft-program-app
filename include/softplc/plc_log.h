/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plc_log.h
 * @brief Minimal levelled logging.
 *
 * Deliberately tiny and dependency-free: this runs in containers where stderr
 * is the log sink, and inside a scan where a logging call must not allocate.
 * Level is set once from SOFTPLC_LOG_LEVEL (error|warn|info|debug).
 */
#ifndef SOFTPLC_PLC_LOG_H
#define SOFTPLC_PLC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum plc_log_level {
    PLC_LOG_LEVEL_ERROR = 0,
    PLC_LOG_LEVEL_WARN,
    PLC_LOG_LEVEL_INFO,
    PLC_LOG_LEVEL_DEBUG
} plc_log_level_t;

/** Read SOFTPLC_LOG_LEVEL and tag every line with @p component. */
void plc_log_init(const char *component);
void plc_log_set_level(plc_log_level_t level);
plc_log_level_t plc_log_level(void);

void plc_log_write(plc_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define PLC_LOG_ERR(...)   plc_log_write(PLC_LOG_LEVEL_ERROR, __VA_ARGS__)
#define PLC_LOG_WARN(...)  plc_log_write(PLC_LOG_LEVEL_WARN,  __VA_ARGS__)
#define PLC_LOG_INFO(...)  plc_log_write(PLC_LOG_LEVEL_INFO,  __VA_ARGS__)
#define PLC_LOG_DEBUG(...) plc_log_write(PLC_LOG_LEVEL_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PLC_LOG_H */
