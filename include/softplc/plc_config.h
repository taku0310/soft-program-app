/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plc_config.h
 * @brief Settings from an INI file, underneath the environment.
 *
 * Every knob in this project was originally an environment variable, because
 * the deployment unit is a container and `docker run -e` needs no mount.  That
 * stops being comfortable once one container holds the core *and* a protocol
 * stack: the settings for both then have to be written into one service block,
 * and there is nowhere to put a comment saying why a value is what it is.
 *
 * So this adds a file underneath, and changes nothing above it:
 *
 *     environment variable   >   config file   >   compiled-in default
 *
 * The environment still wins, so an existing compose file keeps working and a
 * one-off override on the command line still does what it looks like it does.
 *
 * Format:
 *
 *     # A comment.  ';' starts one too.
 *     [core]
 *     instance = line1
 *     role     = adapter        ; none | adapter | scanner
 *
 *     [eip]
 *     interface    = eth0
 *     input_bytes  = 32
 *
 * There is no separate key vocabulary to learn: a key in section `S` is the
 * environment variable `SOFTPLC_S_KEY`, and `[core]` (or no section at all)
 * contributes no prefix, so `[eip] interface` *is* `SOFTPLC_EIP_INTERFACE`.
 * A key already spelled `SOFTPLC_...` is taken verbatim in any section, so
 * anything copied out of a compose file or this project's docs can be pasted
 * straight in.
 *
 * The table is loaded once during start-up, before any thread exists, and is
 * then read-only; the pointers ::plc_cfg_str returns live as long as the
 * process.  Nothing here is safe to call while another thread is running.
 */
#ifndef SOFTPLC_PLC_CONFIG_H
#define SOFTPLC_PLC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "softplc/plc_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where ::plc_config_init looks when SOFTPLC_CONFIG is not set. */
#define PLC_CONFIG_DEFAULT_PATH "/etc/softplc/softplc.conf"

/** Bounded so the table is static: this is start-up configuration, not data. */
#define PLC_CONFIG_MAX_ENTRIES 64
#define PLC_CONFIG_KEY_MAX     64
#define PLC_CONFIG_VALUE_MAX   256

/**
 * @brief Load the config file this process was pointed at.
 *
 * `SOFTPLC_CONFIG` names the file.  When it is set the file must exist and
 * parse - an explicit request that quietly turns into "no configuration" is
 * how a plant ends up running on defaults nobody chose.  When it is unset,
 * ::PLC_CONFIG_DEFAULT_PATH is read if it happens to be there, and its
 * absence is not an error.
 *
 * @param err     receives a one-line diagnostic naming the offending line.
 * @param err_len size of @p err.
 */
plc_status_t plc_config_init(char *err, size_t err_len);

/** Load @p path, which must exist.  Replaces anything loaded before. */
plc_status_t plc_config_load(const char *path, char *err, size_t err_len);

/** Parse from a buffer rather than a file.  Exposed for the tests. */
plc_status_t plc_config_parse(const char *text, char *err, size_t err_len);

/** Drop the table.  For the tests; production loads once and never unloads. */
void plc_config_reset(void);

/** Path actually loaded, or "" when the table came from nowhere. */
const char *plc_config_path(void);

/** The same, phrased for a start-up log line.  Never empty. */
const char *plc_config_source(void);

/**
 * @brief Resolve @p key: environment first, then the file, then @p fallback.
 *
 * @p key may be written in any of the accepted spellings - `SOFTPLC_EIP_INTERFACE`,
 * `eip_interface`, `eip.interface` - and resolves to the same setting.
 */
const char *plc_cfg_str(const char *key, const char *fallback);

/** As ::plc_cfg_str, decimal.  A value that is not a number, or is zero, or
 *  overflows, yields @p fallback: every numeric setting here is a size, a
 *  period or an assembly instance, and none of them may be zero. */
uint32_t plc_cfg_u32(const char *key, uint32_t fallback);

/** As ::plc_cfg_str, for 1/0, true/false, yes/no, on/off. */
int plc_cfg_bool(const char *key, int fallback);

/** Number of entries the file contributed.  For diagnostics. */
size_t plc_config_count(void);

/** Entry @p i, in file order, as the fully qualified key and its raw value. */
plc_status_t plc_config_entry_at(size_t i, const char **key, const char **value);

/**
 * @brief What every entry point does first: load the file, name the log.
 *
 * Returns non-zero when the file could not be read or parsed, and the caller
 * is expected to exit: a process that starts anyway is running on defaults
 * that were not chosen, which is worse than not starting.
 */
int plc_config_bootstrap(const char *component);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PLC_CONFIG_H */
