/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_plc_config.c
 * @brief The settings file: spelling, precedence and what it refuses.
 *
 * Two of these matter more than the rest.  Precedence, because an existing
 * deployment's `environment:` block has to keep winning once a file appears
 * underneath it or the file is a breaking change.  And the refusals, because
 * a settings file that quietly ignores a line an operator wrote is worse than
 * no settings file: the machine then runs on a default nobody chose, and the
 * evidence that it did is a line in a file that looks correct.
 */
#include "test_util.h"

#include <stdlib.h>

#include "softplc/adapter_registry.h"
#include "softplc/plc_config.h"

static void parse_ok(const char *text) {
    char err[256] = "";
    const plc_status_t st = plc_config_parse(text, err, sizeof(err));
    CHECK_EQ_INT(st, PLC_OK);
    if (st != PLC_OK) fprintf(stderr, "      (%s)\n", err);
}

static void parse_fails(const char *text, const char *err_substring) {
    char err[256] = "";
    CHECK(plc_config_parse(text, err, sizeof(err)) != PLC_OK);
    CHECK(strstr(err, err_substring) != NULL);
    if (!strstr(err, err_substring)) {
        fprintf(stderr, "      err was: %s\n", err);
    }
}

/**
 * A key in section S is SOFTPLC_S_KEY; [core] and no section add no prefix.
 *
 * This is the whole reason there is no second vocabulary to learn - and the
 * reason a compose file's keys can be pasted in unchanged, which the last two
 * cases cover.
 */
static void test_section_prefixing(void) {
    parse_ok(
        "instance = top\n"
        "[core]\n"
        "cycle_us = 4000\n"
        "[eip]\n"
        "interface = eth3\n"
        "[scanner]\n"
        "devices = /tmp/d.conf\n"
        "[eip]\n"
        "SOFTPLC_ADAPTERS = loopback\n");

    CHECK_EQ_INT(plc_config_count(), 5);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_INSTANCE", ""), "top") == 0);
    CHECK_EQ_INT(plc_cfg_u32("SOFTPLC_CYCLE_US", 0), 4000);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_EIP_INTERFACE", ""), "eth3") == 0);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_SCANNER_DEVICES", ""), "/tmp/d.conf") == 0);
    /* Fully qualified inside [eip]: taken verbatim, not prefixed again. */
    CHECK(strcmp(plc_cfg_str("SOFTPLC_ADAPTERS", ""), "loopback") == 0);

    /* And a lookup may be spelled any of the ways a key may be written. */
    CHECK(strcmp(plc_cfg_str("eip_interface", ""), "eth3") == 0);
    CHECK(strcmp(plc_cfg_str("eip.interface", ""), "eth3") == 0);
    CHECK(strcmp(plc_cfg_str("EIP-INTERFACE", ""), "eth3") == 0);

    plc_config_reset();
}

/** The environment wins, so adding a file cannot change a running deployment. */
static void test_environment_wins(void) {
    parse_ok("[core]\nrole = adapter\ninstance = from_file\n");

    CHECK(strcmp(plc_cfg_str("SOFTPLC_ROLE", ""), "adapter") == 0);

    setenv("SOFTPLC_ROLE", "scanner", 1);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_ROLE", ""), "scanner") == 0);
    /* Only the overridden key moves; its neighbours still come from the file. */
    CHECK(strcmp(plc_cfg_str("SOFTPLC_INSTANCE", ""), "from_file") == 0);

    /* An empty environment variable is not an override.  Compose writes one
     * for every `KEY:` with no value, and taking that as "" would silently
     * blank a setting the file had. */
    setenv("SOFTPLC_ROLE", "", 1);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_ROLE", ""), "adapter") == 0);

    unsetenv("SOFTPLC_ROLE");
    CHECK(strcmp(plc_cfg_str("SOFTPLC_ROLE", ""), "adapter") == 0);
    plc_config_reset();
}

static void test_comments_and_quoting(void) {
    parse_ok(
        "# leading comment\n"
        "; and this form\n"
        "\n"
        "   [core]   ; trailing comment on a section\n"
        "instance = line1   # trailing comment on a value\n"
        "failsafe = \"hold\"\n"
        "[eip]\n"
        /* '#' mid-token is not a comment: a value may contain punctuation. */
        "interface = eth0#1\n");

    CHECK(strcmp(plc_cfg_str("SOFTPLC_INSTANCE", ""), "line1") == 0);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_FAILSAFE", ""), "hold") == 0);
    CHECK(strcmp(plc_cfg_str("SOFTPLC_EIP_INTERFACE", ""), "eth0#1") == 0);
    plc_config_reset();
}

static void test_refusals(void) {
    parse_fails("[core]\ninstance line1\n", "expected 'key = value'");
    parse_fails("[core\ninstance = x\n", "malformed section");
    parse_fails("[core]\nin$tance = x\n", "bad key");
    /* Repeated across sections that resolve to the same key: the two lines
     * can be pages apart, so taking the last one silently is exactly how the
     * wrong value ends up in service. */
    parse_fails("[core]\ninstance = a\n[eip]\nSOFTPLC_INSTANCE = b\n",
                "already set");
    plc_config_reset();
}

/** A numeric setting is a size, a period or an assembly instance.  None may be
 *  zero, and none may be half a number, so junk falls back rather than
 *  becoming an accidental 0. */
static void test_numeric_rejects_junk(void) {
    parse_ok("[core]\ncycle_us = 12ms\n[eip]\ninput_bytes = 0\n");
    CHECK_EQ_INT(plc_cfg_u32("SOFTPLC_CYCLE_US", 10000), 10000);
    CHECK_EQ_INT(plc_cfg_u32("SOFTPLC_EIP_INPUT_BYTES", 32), 32);
    plc_config_reset();
}

static void test_missing_file(void) {
    char err[256] = "";
    /* Named explicitly and absent: a failure, because a deployment that asked
     * for a file and got defaults is not one anybody intended. */
    CHECK_EQ_INT(plc_config_load("/nonexistent/softplc.conf", err, sizeof(err)),
                 PLC_ERR_NOTFOUND);
    CHECK(strstr(err, "softplc.conf") != NULL);

    /* Not named at all and the default path absent: fine, environment only. */
    unsetenv("SOFTPLC_CONFIG");
    CHECK_EQ_INT(plc_config_init(err, sizeof(err)), PLC_OK);
    plc_config_reset();
}

/**
 * A role resolves to an adapter list, and only to one that exists.
 *
 * This is what the single-container entrypoint reads back out of the binary
 * to decide which stack process to start, so an unknown role has to be
 * distinguishable from a valid one rather than defaulting to something.
 */
static void test_roles(void) {
    plc_adapter_register_builtins();

    CHECK(plc_adapter_role_adapters("none") != NULL);
    CHECK(plc_adapter_role_adapters("nonsense") == NULL);
    CHECK(plc_adapter_role_adapters(NULL) == NULL);

    for (size_t i = 0; ; ++i) {
        const char *role = plc_adapter_role_at(i);
        if (!role) break;
        const char *adapters = plc_adapter_role_adapters(role);
        CHECK(adapters != NULL);
        /* Every protocol a role names must be one this build can create, or
         * the role is a promise the image cannot keep. */
        if (adapters) CHECK(plc_adapter_registry_find(adapters) != NULL);
    }
}

int main(void) {
    test_section_prefixing();
    test_environment_wins();
    test_comments_and_quoting();
    test_refusals();
    test_numeric_rejects_junk();
    test_missing_file();
    test_roles();
    TEST_REPORT("plc_config");
}
