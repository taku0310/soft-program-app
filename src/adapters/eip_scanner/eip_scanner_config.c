/* SPDX-License-Identifier: Apache-2.0 */
#include "eip_scanner_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "softplc/ipc/spsc_ring.h"

#define FIELDS_PER_DEVICE 9

static int parse_u32(const char *tok, unsigned long max, uint32_t *out) {
    char *end = NULL;
    const unsigned long v = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || v > max) return 0;
    *out = (uint32_t)v;
    return 1;
}

static plc_status_t fail(char *err, size_t err_len, unsigned line,
                         const char *what) {
    if (err && err_len) snprintf(err, err_len, "line %u: %s", line, what);
    return PLC_ERR_INVAL;
}

plc_status_t eip_scanner_config_parse(eip_scanner_config_t *cfg,
                                      const char *text,
                                      char *err, size_t err_len) {
    if (!cfg || !text) return PLC_ERR_INVAL;
    memset(cfg, 0, sizeof(*cfg));

    const char *p = text;
    unsigned line_no = 0;

    while (*p) {
        char line[256];
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
        /* A line longer than the buffer would otherwise be silently split into
         * two rows, and the tail could parse as a plausible device. */
        const int overlong = (*p && *p != '\n');
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line[n] = '\0';
        line_no++;

        if (overlong) return fail(err, err_len, line_no, "line too long");

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *tok[FIELDS_PER_DEVICE];
        unsigned count = 0;
        for (char *save = NULL, *t = strtok_r(line, " \t\r", &save);
             t; t = strtok_r(NULL, " \t\r", &save)) {
            if (count < FIELDS_PER_DEVICE) tok[count] = t;
            count++;
        }
        if (count == 0) continue;   /* blank or comment-only */

        if (count != FIELDS_PER_DEVICE) {
            return fail(err, err_len, line_no,
                        "expected 9 fields: ip cfg o2t t2o o2t_len t2o_len "
                        "rpi_us tmo_mult failsafe");
        }
        if (cfg->device_count >= EIP_SCANNER_MAX_DEVICES) {
            return fail(err, err_len, line_no, "too many devices");
        }

        eip_scanner_device_t *d = &cfg->devices[cfg->device_count];
        memset(d, 0, sizeof(*d));

        if (strlen(tok[0]) >= sizeof(d->address)) {
            return fail(err, err_len, line_no, "address too long");
        }
        snprintf(d->address, sizeof(d->address), "%s", tok[0]);

        uint32_t v;
        if (!parse_u32(tok[1], 0xFFFF, &v)) return fail(err, err_len, line_no, "bad config assembly");
        d->config_assembly = (uint16_t)v;
        if (!parse_u32(tok[2], 0xFFFF, &v)) return fail(err, err_len, line_no, "bad O2T assembly");
        d->o2t_assembly = (uint16_t)v;
        if (!parse_u32(tok[3], 0xFFFF, &v)) return fail(err, err_len, line_no, "bad T2O assembly");
        d->t2o_assembly = (uint16_t)v;

        if (!parse_u32(tok[4], PLC_IPC_MAX_FRAME_BYTES, &v)) {
            return fail(err, err_len, line_no, "bad O2T length");
        }
        d->o2t_bytes = (uint16_t)v;
        if (!parse_u32(tok[5], PLC_IPC_MAX_FRAME_BYTES, &v)) {
            return fail(err, err_len, line_no, "bad T2O length");
        }
        d->t2o_bytes = (uint16_t)v;

        if (!parse_u32(tok[6], 0xFFFFFFFFul, &v) || v == 0) {
            return fail(err, err_len, line_no, "bad RPI");
        }
        d->o2t_rpi_us = v;
        d->t2o_rpi_us = v;

        if (!parse_u32(tok[7], 7, &v)) {
            return fail(err, err_len, line_no,
                        "timeout multiplier must be 0..7 (x4 .. x512 of the RPI)");
        }
        d->timeout_multiplier = (uint8_t)v;

        if      (strcmp(tok[8], "hold")  == 0) d->failsafe_policy = PLC_FAILSAFE_HOLD;
        else if (strcmp(tok[8], "clear") == 0) d->failsafe_policy = PLC_FAILSAFE_CLEAR;
        else return fail(err, err_len, line_no, "failsafe must be 'hold' or 'clear'");

        /* Devices are packed in table order, so the %I/%Q layout is readable
         * straight off the file - which is what an engineer wiring a POU to a
         * drive actually needs. */
        d->o2t_offset = cfg->total_o2t_bytes;
        d->t2o_offset = cfg->total_t2o_bytes;

        if (cfg->total_o2t_bytes + d->o2t_bytes > PLC_IPC_MAX_FRAME_BYTES ||
            cfg->total_t2o_bytes + d->t2o_bytes > PLC_IPC_MAX_FRAME_BYTES) {
            return fail(err, err_len, line_no,
                        "aggregate image exceeds the IPC frame limit");
        }
        cfg->total_o2t_bytes += d->o2t_bytes;
        cfg->total_t2o_bytes += d->t2o_bytes;
        cfg->device_count++;
    }

    if (cfg->device_count == 0) {
        if (err && err_len) snprintf(err, err_len, "no devices configured");
        return PLC_ERR_INVAL;
    }
    return PLC_OK;
}

plc_status_t eip_scanner_config_load(eip_scanner_config_t *cfg,
                                     const char *path,
                                     char *err, size_t err_len) {
    if (!cfg || !path) return PLC_ERR_INVAL;

    FILE *f = fopen(path, "re");
    if (!f) {
        if (err && err_len) snprintf(err, err_len, "cannot open %s", path);
        return PLC_ERR_NOTFOUND;
    }

    /* Bounded read: a device table is a few hundred bytes, and this runs on a
     * path a container operator controls, not a trusted one. */
    static char text[16384];
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    const int too_big = !feof(f);
    fclose(f);

    if (too_big) {
        if (err && err_len) snprintf(err, err_len, "%s is larger than 16 KiB", path);
        return PLC_ERR_INVAL;
    }
    text[got] = '\0';
    return eip_scanner_config_parse(cfg, text, err, err_len);
}
