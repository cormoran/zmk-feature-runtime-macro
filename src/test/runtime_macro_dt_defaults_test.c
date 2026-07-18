/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * A minimal in-RAM fake settings backend (same pattern as
 * src/test/runtime_macro_test.c) - needed so a PERSIST-mode write has
 * somewhere to go, and so main()'s settings_load() call has a real load
 * pass to run (which is what triggers the DT-default seed commit callback -
 * see src/runtime_macro_dt_defaults.c). Registered at a lower SYS_INIT
 * priority than this file's own test logic needs to run BEFORE
 * settings_load(), so it just needs to exist before main() calls
 * settings_load(); registering it here (APPLICATION, 98) does that.
 */
#define TEST_SETTINGS_STORAGE_CAPACITY 16

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES + CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 8];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);

    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > sizeof(record->data) || strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -EMSGSIZE;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
                break;
            }
        }
    }
    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};
static struct settings_store test_settings_store = {.cs_itf = &test_settings_itf};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return 0;
}

/*
 * A user-created (persisted) macro, created BEFORE settings_load() runs so
 * it is a genuine persisted entry once the fake backend's save lands - this
 * proves the DT-default seed step (which fires from settings_load()'s
 * commit callback, after this write's persisted record has already been
 * loaded and bound) does NOT clobber a persisted user entry that happens to
 * share a name with a devicetree default (see tests/dt-defaults/native_sim.keymap:
 * "Hi" is both a DT default's name and, here, deliberately pre-created by
 * the user with different content).
 */
static int seed_user_macro_same_name_as_dt_default(void) {
    /* A single tap of usage 0x1D ('z') - deliberately NOT the DT default's own
     * "hi" text, so playing this back after settings_load() proves the
     * persisted user content won, not the devicetree default. Created first
     * (before any DT default seed runs), so it deterministically claims slot
     * 0 - matching tests/dt-defaults/native_sim.keymap's `&rmacro 0` binding. */
    uint8_t body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION, ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE, 1,
                      0x1D};

    return zmk_runtime_macro_create("Hi", body, sizeof(body), ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
                                    NULL);
}

static int runtime_macro_dt_defaults_pre_load_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        LOG_ERR("Failed to init fake settings backend: %d", ret);
        return ret;
    }

    ret = seed_user_macro_same_name_as_dt_default();
    if (ret < 0) {
        LOG_ERR("Failed to seed pre-existing \"Hi\" macro: %d", ret);
        return ret;
    }

    return 0;
}

/* Must run before main()'s settings_load() (always later - see
 * src/runtime_macro_dt_defaults.c's h_commit doc comment), but the specific
 * priority relative to other APPLICATION-level init doesn't matter here
 * (unlike the old design, there is no more SYS_INIT-ordering dependency on
 * behavior_local_id_init - the DT-default seed itself already accounts for
 * that by running from settings_load(), which is always after every
 * SYS_INIT stage). */
SYS_INIT(runtime_macro_dt_defaults_pre_load_init, APPLICATION, 98);

/*
 * Runs after main()'s settings_load() to inspect the result - there is no
 * SYS_INIT stage that fires after main(), so this file's assertions run
 * from settings_load()'s own commit phase too, chained after the module's
 * own DT-default seed handler via a higher cprio (larger cprio number = runs
 * later within the same settings_commit_subtree pass - see subsys/settings/
 * src/settings.c's ascending-cprio commit loop).
 */
static int runtime_macro_dt_defaults_test_verify(void) {
    /* The pre-existing persisted "Hi" macro must NOT have been overwritten
     * by the DT default of the same name. */
    uint8_t encoded[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t encoded_size = 0;
    int ret = zmk_runtime_macro_read("Hi", encoded, sizeof(encoded), &encoded_size);
    if (ret < 0 || encoded_size != 4 || encoded[3] != 0x1D) {
        LOG_ERR("Persisted \"Hi\" macro was not preserved: ret=%d size=%zu", ret, encoded_size);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_did_not_overwrite_persisted_entry");

    /* The DT default declared with no user-created counterpart ("Bindings")
     * must have been installed. */
    ret = zmk_runtime_macro_read("Bindings", encoded, sizeof(encoded), &encoded_size);
    if (ret < 0 || encoded_size == 0) {
        LOG_ERR("DT default \"Bindings\" was not installed: ret=%d size=%zu", ret, encoded_size);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_installed name=Bindings size=%u",
            (unsigned)encoded_size);

    /* zmk_runtime_macro_default_encode (used by the RPC reset-to-default) must
     * reproduce exactly the body the seed installed for a name with a DT
     * default... */
    uint8_t default_body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t default_size = 0;
    ret = zmk_runtime_macro_default_encode("Bindings", default_body, sizeof(default_body),
                                           &default_size);
    if (ret < 0 || default_size != encoded_size ||
        memcmp(default_body, encoded, encoded_size) != 0) {
        LOG_ERR("default_encode(\"Bindings\") mismatch: ret=%d size=%zu (installed=%zu)", ret,
                default_size, encoded_size);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_default_encode_matches_installed");

    /* ...and must report -ENOENT for a name with no DT default (the reset RPC
     * turns that into a delete). */
    ret = zmk_runtime_macro_default_encode("No Such Default", default_body, sizeof(default_body),
                                           &default_size);
    if (ret != -ENOENT) {
        LOG_ERR("default_encode of a name with no DT default should be -ENOENT: ret=%d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_default_encode_absent_is_enoent");

    /* A user macro created AFTER settings_load() (i.e. during normal RPC use)
     * must coexist fine alongside the already-seeded DT defaults. */
    uint8_t user_body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};
    ret = zmk_runtime_macro_create("User Macro", user_body, sizeof(user_body),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        LOG_ERR("Failed to create independent user macro alongside DT defaults: %d", ret);
        return ret;
    }
    ret = zmk_runtime_macro_read("User Macro", encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        LOG_ERR("Independent user macro did not survive alongside DT defaults: %d", ret);
        return ret;
    }
    LOG_INF("PASS: runtime_macro_user_and_dt_default_coexist");

    /* The oversized DT default ("Oversized", see
     * tests/dt-defaults/native_sim.keymap) must have been skipped cleanly,
     * without preventing the other two defaults above from installing. */
    ret = zmk_runtime_macro_read("Oversized", encoded, sizeof(encoded), &encoded_size);
    if (ret != -ENOENT) {
        LOG_ERR("Oversized DT default should not have installed: ret=%d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_oversized_skipped");

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(runtime_macro_dt_defaults_test_verify,
                                          "runtime_macro_dt_defaults_test_verify", NULL, NULL,
                                          runtime_macro_dt_defaults_test_verify, NULL, 1);
