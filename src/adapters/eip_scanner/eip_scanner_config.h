/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_config.h
 * @brief The device table a Scanner needs, and its parser.
 *
 * The Adapter role needs a handful of environment variables.  A Scanner needs
 * a table: per remote device an address, three assembly instances, two image
 * sizes, two RPIs and a timeout multiplier.  That does not fit flat env vars
 * legibly, so it lives in a mounted file.
 *
 * The format is deliberately line-based rather than JSON or YAML: this project
 * has no third-party runtime dependencies and a device table is not worth
 * acquiring one for.  A parser for it is about sixty lines.
 *
 *     # ip              cfg  o2t  t2o  o2t_len  t2o_len  rpi_us   failsafe
 *     192.168.1.10      151  150  100  32       32       10000    hold
 *     192.168.1.11      151  150  100  8        8        10000    clear
 *
 * Columns are whitespace separated, '#' starts a comment, blank lines are
 * skipped.  Every field is required; a short row is an error rather than a
 * defaulted row, because a silently defaulted RPI or image size is a plant
 * fault waiting to happen.
 */
#ifndef SOFTPLC_EIP_SCANNER_CONFIG_H
#define SOFTPLC_EIP_SCANNER_CONFIG_H

#include <stdint.h>

#include "softplc/plc_status.h"
#include "softplc/protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Remote devices one scanner process may drive.  Sized well above the four
 *  this deployment needs, but bounded: the table is a fixed array in shared
 *  memory, so it cannot grow at runtime. */
#define EIP_SCANNER_MAX_DEVICES 16

#define EIP_SCANNER_ADDR_MAX 46   /* fits an IPv4 or IPv6 literal */

typedef struct eip_scanner_device {
    char     address[EIP_SCANNER_ADDR_MAX];

    uint16_t config_assembly;   /**< connection path: configuration */
    uint16_t o2t_assembly;      /**< originator -> target: PLC %Q goes here  */
    uint16_t t2o_assembly;      /**< target -> originator: PLC %I comes here */

    uint16_t o2t_bytes;         /**< size of the O->T image for this device */
    uint16_t t2o_bytes;         /**< size of the T->O image for this device */

    uint32_t o2t_rpi_us;        /**< requested packet interval, O->T */
    uint32_t t2o_rpi_us;        /**< requested packet interval, T->O */

    /** Per device, because one drive dropping should not be forced to mean the
     *  same thing as another.  Applied inside the scanner adapter, to this
     *  device's slice only. */
    plc_failsafe_policy_t failsafe_policy;

    /* Filled in by the loader: where this device's slices sit inside the
     * adapter's aggregate images.  Devices are packed in table order. */
    uint32_t o2t_offset;
    uint32_t t2o_offset;
} eip_scanner_device_t;

typedef struct eip_scanner_config {
    eip_scanner_device_t devices[EIP_SCANNER_MAX_DEVICES];
    uint32_t             device_count;

    uint32_t total_o2t_bytes;   /**< sum of the device O->T slices */
    uint32_t total_t2o_bytes;   /**< sum of the device T->O slices */
} eip_scanner_config_t;

/**
 * @brief Parse a device table.
 *
 * @param err     receives a one-line diagnostic naming the offending line.
 * @param err_len size of @p err.
 * @return ::PLC_OK, or ::PLC_ERR_INVAL with @p err filled in.
 */
plc_status_t eip_scanner_config_load(eip_scanner_config_t *cfg,
                                     const char *path,
                                     char *err, size_t err_len);

/** Parse from a buffer rather than a file.  Exposed for the tests. */
plc_status_t eip_scanner_config_parse(eip_scanner_config_t *cfg,
                                      const char *text,
                                      char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_EIP_SCANNER_CONFIG_H */
