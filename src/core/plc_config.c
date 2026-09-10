/* SPDX-License-Identifier: Apache-2.0 */
#include "softplc/plc_config.h"

#include "softplc/plc_log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A settings file is start-up configuration, not data: anything larger than
 * this is a mistake, and reading it into a fixed buffer keeps the parser
 * allocation-free like the rest of the project. */
#define CONFIG_MAX_BYTES 65536

typedef struct entry {
    char key[PLC_CONFIG_KEY_MAX];
    char value[PLC_CONFIG_VALUE_MAX];
} entry_t;

static entry_t g_entries[PLC_CONFIG_MAX_ENTRIES];
static size_t  g_count;
static char    g_path[512];

static void fail(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void fail(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* --- lexing -------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = '\0';
    return s;
}

/**
 * Cut the line at an unquoted comment.
 *
 * '#' and ';' only start a comment at the start of the line or after
 * whitespace, so a value may contain either - a path or a password is not
 * obliged to avoid punctuation just because the parser is simple.
 */
static void strip_comment(char *line) {
    int quote = 0;
    for (char *p = line; *p; ++p) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if ((*p == '#' || *p == ';') &&
                   (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
    }
}

/** Drop one layer of matched quotes, so a value may keep its own whitespace. */
static void unquote(char *s) {
    const size_t n = strlen(s);
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/**
 * Normalise one identifier into the environment-variable spelling.
 *
 * `-`, `.` and spaces become `_` and letters are folded up, so `eip.input-bytes`
 * and `EIP_INPUT_BYTES` are the same setting.  Anything else is rejected
 * rather than silently mangled into a key that will never match.
 */
static plc_status_t normalise(char *out, size_t out_len, const char *in) {
    size_t n = 0;
    for (const char *p = in; *p; ++p) {
        char c = *p;
        if (c == '-' || c == '.' || c == ' ') c = '_';
        else if (islower((unsigned char)c)) c = (char)toupper((unsigned char)c);
        if (!(isupper((unsigned char)c) || isdigit((unsigned char)c) || c == '_'))
            return PLC_ERR_INVAL;
        if (n + 1 >= out_len) return PLC_ERR_NOMEM;
        out[n++] = c;
    }
    out[n] = '\0';
    return n ? PLC_OK : PLC_ERR_INVAL;
}

/**
 * Build the fully qualified key for @p key inside @p section.
 *
 * The rule is mechanical, which is the point: a key in section `S` is
 * `SOFTPLC_S_KEY`, `[core]` and no section contribute no prefix, and a key
 * already carrying the prefix is left alone wherever it appears.  That last
 * case is what lets a compose `environment:` block be pasted in unchanged.
 */
static plc_status_t qualify(char *out, size_t out_len,
                            const char *section, const char *key) {
    char k[PLC_CONFIG_KEY_MAX];
    plc_status_t st = normalise(k, sizeof(k), key);
    if (st != PLC_OK) return st;

    if (strncmp(k, "SOFTPLC_", 8) == 0) {
        return (snprintf(out, out_len, "%s", k) < (int)out_len)
            ? PLC_OK : PLC_ERR_NOMEM;
    }

    char s[PLC_CONFIG_KEY_MAX] = "";
    if (section && *section) {
        st = normalise(s, sizeof(s), section);
        if (st != PLC_OK) return st;
        if (strcmp(s, "CORE") == 0 || strcmp(s, "SOFTPLC") == 0) s[0] = '\0';
    }

    const int n = *s ? snprintf(out, out_len, "SOFTPLC_%s_%s", s, k)
                     : snprintf(out, out_len, "SOFTPLC_%s", k);
    return (n > 0 && n < (int)out_len) ? PLC_OK : PLC_ERR_NOMEM;
}

/* --- parsing ------------------------------------------------------------- */

static entry_t *find(const char *qualified_key) {
    for (size_t i = 0; i < g_count; ++i) {
        if (strcmp(g_entries[i].key, qualified_key) == 0) return &g_entries[i];
    }
    return NULL;
}

plc_status_t plc_config_parse(const char *text, char *err, size_t err_len) {
    if (!text) return PLC_ERR_INVAL;
    if (err && err_len) err[0] = '\0';

    g_count = 0;

    char section[PLC_CONFIG_KEY_MAX] = "";
    char line[PLC_CONFIG_VALUE_MAX + PLC_CONFIG_KEY_MAX + 8];
    int  lineno = 0;

    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        lineno++;

        if (len >= sizeof(line)) {
            fail(err, err_len, "line %d: longer than %zu bytes",
                 lineno, sizeof(line) - 1);
            return PLC_ERR_INVAL;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;

        strip_comment(line);
        char *s = trim(line);
        if (!*s) continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close || *trim(close + 1) != '\0') {
                fail(err, err_len, "line %d: malformed section header", lineno);
                return PLC_ERR_INVAL;
            }
            *close = '\0';
            char *name = trim(s + 1);
            if (normalise(section, sizeof(section), name) != PLC_OK) {
                fail(err, err_len, "line %d: bad section name '%s'", lineno, name);
                return PLC_ERR_INVAL;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            fail(err, err_len, "line %d: expected 'key = value', got '%s'",
                 lineno, s);
            return PLC_ERR_INVAL;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        unquote(val);

        char qualified[PLC_CONFIG_KEY_MAX];
        if (qualify(qualified, sizeof(qualified), section, key) != PLC_OK) {
            fail(err, err_len, "line %d: bad key '%s'", lineno, key);
            return PLC_ERR_INVAL;
        }
        if (strlen(val) >= PLC_CONFIG_VALUE_MAX) {
            fail(err, err_len, "line %d: value for %s exceeds %d bytes",
                 lineno, qualified, PLC_CONFIG_VALUE_MAX - 1);
            return PLC_ERR_INVAL;
        }

        /* A repeated key is a mistake, not an override: the two spellings may
         * be in different sections and pages apart, and taking the last one
         * silently is how a machine ends up running the setting nobody was
         * looking at. */
        if (find(qualified)) {
            fail(err, err_len, "line %d: %s is already set", lineno, qualified);
            return PLC_ERR_INVAL;
        }
        if (g_count == PLC_CONFIG_MAX_ENTRIES) {
            fail(err, err_len, "line %d: more than %d settings",
                 lineno, PLC_CONFIG_MAX_ENTRIES);
            return PLC_ERR_INVAL;
        }

        snprintf(g_entries[g_count].key, PLC_CONFIG_KEY_MAX, "%s", qualified);
        snprintf(g_entries[g_count].value, PLC_CONFIG_VALUE_MAX, "%s", val);
        g_count++;
    }

    return PLC_OK;
}

plc_status_t plc_config_load(const char *path, char *err, size_t err_len) {
    if (!path || !*path) return PLC_ERR_INVAL;
    if (err && err_len) err[0] = '\0';

    FILE *f = fopen(path, "re");
    if (!f) {
        fail(err, err_len, "%s: %s", path, strerror(errno));
        return (errno == ENOENT) ? PLC_ERR_NOTFOUND : PLC_ERR_IO;
    }

    char *buf = malloc(CONFIG_MAX_BYTES);
    if (!buf) { fclose(f); return PLC_ERR_NOMEM; }

    const size_t n = fread(buf, 1, CONFIG_MAX_BYTES - 1, f);
    const int too_big = !feof(f) && !ferror(f);
    const int read_err = ferror(f);
    fclose(f);

    if (read_err) {
        fail(err, err_len, "%s: read failed", path);
        free(buf);
        return PLC_ERR_IO;
    }
    if (too_big) {
        fail(err, err_len, "%s: larger than %d bytes", path, CONFIG_MAX_BYTES);
        free(buf);
        return PLC_ERR_INVAL;
    }
    buf[n] = '\0';

    const plc_status_t st = plc_config_parse(buf, err, err_len);
    free(buf);
    if (st != PLC_OK) {
        g_count = 0;
        g_path[0] = '\0';
        return st;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);
    return PLC_OK;
}

plc_status_t plc_config_init(char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';

    const char *explicit_path = getenv("SOFTPLC_CONFIG");
    if (explicit_path && *explicit_path) {
        /* Asked for by name: a missing file is a failure, not a fallback. */
        return plc_config_load(explicit_path, err, err_len);
    }

    const plc_status_t st = plc_config_load(PLC_CONFIG_DEFAULT_PATH, err, err_len);
    if (st == PLC_ERR_NOTFOUND) {
        if (err && err_len) err[0] = '\0';
        return PLC_OK;   /* environment-only, which is the original behaviour */
    }
    return st;
}

void plc_config_reset(void) {
    g_count = 0;
    g_path[0] = '\0';
}

const char *plc_config_path(void) { return g_path; }

const char *plc_config_source(void) {
    return g_count ? g_path : "environment only";
}

size_t plc_config_count(void) { return g_count; }

plc_status_t plc_config_entry_at(size_t i, const char **key, const char **value) {
    if (i >= g_count) return PLC_ERR_NOTFOUND;
    if (key)   *key   = g_entries[i].key;
    if (value) *value = g_entries[i].value;
    return PLC_OK;
}

/* --- lookup -------------------------------------------------------------- */

const char *plc_cfg_str(const char *key, const char *fallback) {
    if (!key) return fallback;

    char qualified[PLC_CONFIG_KEY_MAX];
    if (qualify(qualified, sizeof(qualified), NULL, key) != PLC_OK) return fallback;

    const char *env = getenv(qualified);
    if (env && *env) return env;

    const entry_t *e = find(qualified);
    return (e && e->value[0]) ? e->value : fallback;
}

uint32_t plc_cfg_u32(const char *key, uint32_t fallback) {
    const char *v = plc_cfg_str(key, NULL);
    if (!v) return fallback;
    char *end = NULL;
    errno = 0;
    const unsigned long n = strtoul(v, &end, 10);
    if (end == v || errno == ERANGE || n == 0 || n > 0xFFFFFFFFul) return fallback;
    while (*end == ' ' || *end == '\t') end++;
    return *end ? fallback : (uint32_t)n;
}

int plc_cfg_bool(const char *key, int fallback) {
    const char *v = plc_cfg_str(key, NULL);
    if (!v) return fallback;
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0) return 1;
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0) return 0;
    return fallback;
}

/* --- start-up ------------------------------------------------------------ */

int plc_config_bootstrap(const char *component) {
    char err[256];
    /* Before plc_log_init(), because the file may carry the log level that
     * governs everything printed after this point. */
    const plc_status_t st = plc_config_init(err, sizeof(err));
    plc_log_init(component);

    if (st != PLC_OK) {
        PLC_LOG_ERR("configuration: %s", err[0] ? err : plc_strerror(st));
        return -1;
    }
    /* Nothing is logged on success: the entrypoint runs this binary three
     * times to read settings back out of it, and a start-up banner from each
     * of those would bury the one line that is actually about the run.  Each
     * process names its configuration source in its own start-up line, via
     * plc_config_source(). */
    return 0;
}
