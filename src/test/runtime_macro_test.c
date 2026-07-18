/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * A minimal in-RAM fake settings backend, mirroring
 * zmk-feature-custom-settings' own src/test/custom_settings_test.c: this
 * build has no real flash-backed settings store (native_sim test builds use
 * CONFIG_SETTINGS_NONE), so without registering *some* store,
 * ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST would fail with -ENOENT
 * (settings_save_one() has nowhere to write). Registering this before any
 * persist-mode write below makes the persist -> memory-overwrite -> discard
 * tests exercise a real store/reload round trip instead of only RAM state -
 * this is the test pattern that caught a real key-collision bug in the
 * pre-keyspace design (see docs/design/keyspace-macros.md), so it is kept
 * even though the storage layer underneath changed completely.
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

/* Finds a macro's underlying custom setting by name, using only generic
 * (already-public) custom-settings APIs - a test-only convenience so this
 * file can call zmk_custom_setting_discard() on it, mirroring what the
 * pre-keyspace test did via zmk_custom_setting_find(). */
static const struct zmk_custom_setting *find_macro_setting(const char *name) {
    char key[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 16];
    snprintf(key, sizeof(key), ZMK_RUNTIME_MACRO_KEY_PREFIX "%s", name);

    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, key);
    if (!keyspace) {
        return NULL;
    }
    return zmk_custom_setting_keyspace_find(keyspace, key);
}

/*
 * Emits one ZMK_RUNTIME_MACRO_OP_DELAY op (opcode + uvarint delay_ms) whose
 * encoded length is exactly `op_size` bytes (2..6: a 1-5 byte uvarint plus
 * the opcode byte), using a delay_ms value picked purely for its uvarint
 * width - these bodies are only ever validated/stored/read back in tests
 * below, never played back, so the actual delay value is irrelevant.
 */
static void emit_delay_of_size(uint8_t *buf, size_t op_size) {
    static const uint32_t value_for_size[] = {
        [2] = 0,          // 1-byte uvarint (0-127)
        [3] = 200,        // 2-byte uvarint (128-16383)
        [4] = 20000,      // 3-byte uvarint (16384-2097151)
        [5] = 3000000,    // 4-byte uvarint (2097152-268435455)
        [6] = 0xffffffff, // 5-byte uvarint (268435456-4294967295)
    };

    buf[0] = ZMK_RUNTIME_MACRO_OP_DELAY;
    uint32_t value = value_for_size[op_size];
    for (size_t i = 1; i < op_size; i++) {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        buf[i] = (i + 1 < op_size) ? (byte | 0x80) : byte;
    }
}

/*
 * Builds a body of a chain of DELAY ops that encodes to exactly `total_size`
 * bytes (1 format-version byte + a decomposition of the remaining
 * `total_size - 1` bytes into ops of size 2-6, never leaving an unfillable
 * 1-byte remainder) and decodes to at most `total_size / 2` queue items -
 * comfortably under CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE's default of 64 even
 * at CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES's default of 256.
 */
static size_t build_delay_padding_body(uint8_t *buf, size_t total_size) {
    buf[0] = ZMK_RUNTIME_MACRO_FORMAT_VERSION;
    size_t offset = 1;
    size_t remaining = total_size - 1;

    while (remaining >= 8) {
        emit_delay_of_size(&buf[offset], 6);
        offset += 6;
        remaining -= 6;
    }

    if (remaining == 7) {
        /* 6+1 would leave an unfillable 1-byte op; split as 2+5 instead. */
        emit_delay_of_size(&buf[offset], 2);
        offset += 2;
        emit_delay_of_size(&buf[offset], 5);
        offset += 5;
    } else if (remaining > 0) {
        emit_delay_of_size(&buf[offset], remaining);
        offset += remaining;
    }

    return offset;
}

/* Created FIRST (before any other macro in this test binary), so it is
 * guaranteed to claim keyspace slot 0 - matching tests/test/native_sim.keymap's
 * `&rmacro 0` binding and tests/test/keycode_events.snapshot. Its body is
 * intentionally > 64 bytes (the old settings-carrier limit) to exercise
 * playback of a macro that only fits through the shared pool / read_into
 * path. 62 identical packed key taps of usage 0x04 ('a') encode to 65 bytes
 * (1 + 1 + 1 + 62) and decode to 62 queued taps - well under
 * CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE's default of 64. */
#define PLAYBACK_TEST_KEY_COUNT 62

static int seed_playback_macro(void) {
    uint8_t encoded[3 + PLAYBACK_TEST_KEY_COUNT];

    encoded[0] = ZMK_RUNTIME_MACRO_FORMAT_VERSION;
    encoded[1] = ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE;
    encoded[2] = PLAYBACK_TEST_KEY_COUNT;
    memset(&encoded[3], 0x04, PLAYBACK_TEST_KEY_COUNT);

    BUILD_ASSERT(sizeof(encoded) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
                 "Playback test body must exceed the old 64-byte carrier limit");

    uint32_t slot = 0;
    int ret = zmk_runtime_macro_create("Long Macro", encoded, sizeof(encoded),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &slot);
    if (ret < 0) {
        LOG_ERR("Failed to seed runtime macro playback test data: %d", ret);
        return ret;
    }
    if (slot != 0) {
        LOG_ERR("Playback test macro must land in slot 0 (got %u) - keymap binding depends on "
                "it being created first",
                slot);
        return -EINVAL;
    }

    return 0;
}

/* Round-trips a body well above the old 64-byte settings carrier through
 * create -> read on a fresh name, verifying both size, content, and the
 * name -> slot -> name resolution helpers. */
static int test_round_trip_large_body(void) {
    uint8_t body[150];
    size_t body_size = build_delay_padding_body(body, sizeof(body));

    uint32_t slot = 0;
    int ret = zmk_runtime_macro_create("Round Trip", body, body_size,
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &slot);
    if (ret < 0) {
        LOG_ERR("Round-trip create failed: %d", ret);
        return ret;
    }

    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read("Round Trip", read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != body_size || memcmp(read_back, body, body_size) != 0) {
        LOG_ERR("Round-trip mismatch: ret=%d size=%zu", ret, read_size);
        return -EINVAL;
    }

    char name_back[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    ret = zmk_runtime_macro_name_for_slot(slot, name_back, sizeof(name_back));
    if (ret < 0 || strcmp(name_back, "Round Trip") != 0) {
        LOG_ERR("Slot -> name resolution mismatch: ret=%d name=%s", ret, name_back);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_round_trip_large_body size=%u slot=%u", (unsigned)read_size, slot);
    return 0;
}

/* Exactly CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES must be accepted; one byte more
 * must fail with -EMSGSIZE and must not disturb the macro's stored body. */
static int test_max_bytes_boundary(void) {
    uint8_t at_max[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t at_max_size = build_delay_padding_body(at_max, sizeof(at_max));

    int ret = zmk_runtime_macro_create("At Max", at_max, at_max_size,
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        LOG_ERR("Create at exactly MAX_BYTES failed: %d", ret);
        return ret;
    }
    LOG_INF("PASS: runtime_macro_max_bytes_boundary_ok size=%u", (unsigned)at_max_size);

    uint8_t over_max[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES + 1];
    size_t over_max_size = build_delay_padding_body(over_max, sizeof(over_max));

    ret = zmk_runtime_macro_write("At Max", over_max, over_max_size,
                                  ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Write over MAX_BYTES returned %d, expected -EMSGSIZE", ret);
        return -EINVAL;
    }

    /* The rejected write must not have touched the previously stored body. */
    uint8_t read_back[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read("At Max", read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != at_max_size) {
        LOG_ERR("Macro changed after a rejected oversized write: ret=%d size=%zu", ret, read_size);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_max_bytes_boundary_rejected");
    return 0;
}

/*
 * A name made entirely of digits must not be mistaken for anything else by
 * the create/find/delete path - see docs/design/keyspace-macros.md's note on
 * this having been a real collision risk in an earlier custom-settings
 * revision (a stored "<key>/<digits>" name used to be parsed as a legacy
 * array element). Verified by testing, not assumed.
 */
static int test_all_digit_name(void) {
    uint8_t body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};

    int ret = zmk_runtime_macro_create("42", body, sizeof(body),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        LOG_ERR("Failed to create all-digit macro name \"42\": %d", ret);
        return ret;
    }

    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read("42", read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != sizeof(body)) {
        LOG_ERR("Failed to read back all-digit macro name \"42\": ret=%d size=%zu", ret, read_size);
        return -EINVAL;
    }

    ret = zmk_runtime_macro_delete("42");
    if (ret < 0) {
        LOG_ERR("Failed to delete all-digit macro name \"42\": %d", ret);
        return ret;
    }

    LOG_INF("PASS: runtime_macro_all_digit_name_ok");
    return 0;
}

/* Create/write/rename/delete by name, exercising the full lifecycle. */
static int test_name_lifecycle(void) {
    uint8_t body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};

    int ret = zmk_runtime_macro_create("Lifecycle", body, sizeof(body),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        return ret;
    }

    /* Creating a second macro under the same name must fail. */
    ret = zmk_runtime_macro_create("Lifecycle", body, sizeof(body),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret != -EEXIST) {
        LOG_ERR("Duplicate create returned %d, expected -EEXIST", ret);
        return -EINVAL;
    }

    ret = zmk_runtime_macro_rename("Lifecycle", "Renamed", ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Rename failed: %d", ret);
        return ret;
    }

    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read("Lifecycle", read_back, sizeof(read_back), &read_size);
    if (ret != -ENOENT) {
        LOG_ERR("Old name still resolves after rename: ret=%d", ret);
        return -EINVAL;
    }
    ret = zmk_runtime_macro_read("Renamed", read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != sizeof(body)) {
        LOG_ERR("Renamed macro did not carry over its body: ret=%d size=%zu", ret, read_size);
        return -EINVAL;
    }

    ret = zmk_runtime_macro_delete("Renamed");
    if (ret < 0) {
        LOG_ERR("Delete failed: %d", ret);
        return ret;
    }
    ret = zmk_runtime_macro_delete("Renamed");
    if (ret != -ENOENT) {
        LOG_ERR("Deleting an already-deleted macro returned %d, expected -ENOENT", ret);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_name_lifecycle");
    return 0;
}

/*
 * Fills the shared name+body pool with MAX_BYTES bodies (creating fresh
 * names, since macros are addressed by name now - not fixed slot indices)
 * until a create fails with -ENOSPC, then deletes one and confirms the retry
 * succeeds - and, in the same step, exercises persist -> memory-mode
 * overwrite -> discard on that large body via the real fake settings backend
 * (the pooled-value analogue of custom-settings' own discard/ensure_region
 * path). This is the exact test pattern (persist -> discard -> reload) that
 * caught a real key-collision bug in the pre-keyspace design.
 */
#define POOL_FILL_ATTEMPTS 6

static int test_pool_budget_and_discard(void) {
    uint8_t body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t body_size = build_delay_padding_body(body, sizeof(body));

    char first_success_name[32] = {0};
    char enospc_name[32] = {0};
    bool have_success = false;
    bool have_enospc = false;

    for (int i = 0; i < POOL_FILL_ATTEMPTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Pool Fill %d", i);

        int ret = zmk_runtime_macro_create(name, body, body_size,
                                           ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
        if (ret == -ENOSPC) {
            strcpy(enospc_name, name);
            have_enospc = true;
            break;
        }
        if (ret < 0) {
            LOG_ERR("Unexpected error creating pool-fill macro %s: %d", name, ret);
            return ret;
        }
        if (!have_success) {
            strcpy(first_success_name, name);
            have_success = true;
        }
    }

    if (!have_enospc || !have_success) {
        LOG_ERR("Pool budget test did not observe -ENOSPC within %d attempts (pool_used=%zu/%zu) "
                "- is CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES large enough to hold every attempt?",
                POOL_FILL_ATTEMPTS, zmk_runtime_macro_pool_used(), zmk_runtime_macro_pool_total());
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_pool_exhausted");

    /* Free the first macro that succeeded and retry under the name that hit
     * -ENOSPC. */
    int ret = zmk_runtime_macro_delete(first_success_name);
    if (ret < 0) {
        LOG_ERR("Failed to delete %s: %d", first_success_name, ret);
        return ret;
    }

    ret = zmk_runtime_macro_create(enospc_name, body, body_size,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, NULL);
    if (ret < 0) {
        LOG_ERR("Retry create after freeing a slot failed: %d", ret);
        return ret;
    }
    LOG_INF("PASS: runtime_macro_pool_freed_slot_retry_succeeded");

    /* Persist -> overwrite in memory mode -> discard restores the persisted
     * (> 64 byte) body, exercising the pooled-body analogue of custom-settings'
     * discard/ensure_region path. */
    const struct zmk_custom_setting *macro_setting = find_macro_setting(enospc_name);
    if (!macro_setting) {
        LOG_ERR("Could not find macro setting for %s", enospc_name);
        return -ENODEV;
    }

    /* A trivially valid (empty-effect) body: just the format version byte. */
    uint8_t overwrite_body[1] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};
    ret = zmk_runtime_macro_write(enospc_name, overwrite_body, sizeof(overwrite_body),
                                  ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Memory-mode overwrite before discard failed: %d", ret);
        return ret;
    }

    ret = zmk_custom_setting_discard(macro_setting);
    if (ret < 0) {
        LOG_ERR("Discard of pooled macro setting failed: %d", ret);
        return ret;
    }

    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read(enospc_name, read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != body_size || memcmp(read_back, body, body_size) != 0) {
        LOG_ERR("Discard did not restore the persisted large body: ret=%d size=%zu", ret,
                read_size);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_pool_discard_restores_persisted_body size=%u",
            (unsigned)read_size);
    return 0;
}

/* tap_ms is a plain (non-keyspace) scalar setting, unaffected by the
 * macro-storage migration - a quick persist/discard round trip proves it
 * still works through the real fake settings backend. */
static int test_tap_ms_persist_discard(void) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(500);
    int ret = zmk_custom_setting_write_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                              ZMK_RUNTIME_MACRO_TAP_MS_KEY, &value,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("tap_ms persist write failed: %d", ret);
        return ret;
    }

    struct zmk_custom_setting_value overwrite = ZMK_CUSTOM_SETTING_VALUE_INT32(999);
    ret = zmk_custom_setting_write_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                          ZMK_RUNTIME_MACRO_TAP_MS_KEY, &overwrite,
                                          ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *tap_ms_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_TAP_MS_KEY);
    if (!tap_ms_setting) {
        return -ENODEV;
    }
    ret = zmk_custom_setting_discard(tap_ms_setting);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value read_back;
    ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                         ZMK_RUNTIME_MACRO_TAP_MS_KEY, &read_back);
    if (ret < 0 || read_back.int32_value != 500) {
        LOG_ERR("tap_ms discard did not restore persisted value: ret=%d value=%d", ret,
                ret < 0 ? -1 : read_back.int32_value);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_tap_ms_persist_discard");
    return 0;
}

/*
 * The four operational Studio RPCs (get/set-step-count/set-step/append-step
 * in src/studio/runtime_macro_handler.c) address a macro by slot number, not
 * name: the handler resolves slot -> name via zmk_runtime_macro_name_for_slot()
 * once and then calls the same name-keyed zmk_runtime_macro_write()/read()
 * this test file already exercises everywhere else - see that function's
 * resolve_slot_name() helper. This test proves that resolution path end to
 * end: after creating a macro by name and noting its assigned slot (exactly
 * how a client is expected to discover it, from the create/list response),
 * every subsequent operation below uses ONLY the slot - the name is never
 * referenced again - then also exercises the two slot-resolution failure
 * modes the handler surfaces as distinct RPC errors: an unbound (freed)
 * in-range slot (-ENOENT: "no macro at that slot") and an out-of-range slot
 * (-ERANGE: "slot out of range").
 */
static int test_slot_addressing(void) {
    uint8_t initial_body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};

    uint32_t slot = 0;
    int ret = zmk_runtime_macro_create("Slot Addressed", initial_body, sizeof(initial_body),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &slot);
    if (ret < 0) {
        LOG_ERR("Slot-addressing create failed: %d", ret);
        return ret;
    }

    /* From here on only `slot` is used - mirroring the RPC handler's
     * resolve-then-call-by-name pattern. */
    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    ret = zmk_runtime_macro_name_for_slot(slot, name, sizeof(name));
    if (ret < 0 || strcmp(name, "Slot Addressed") != 0) {
        LOG_ERR("Slot -> name resolution failed: ret=%d name=%s", ret, name);
        return -EINVAL;
    }

    uint8_t new_body[8];
    size_t new_body_size = build_delay_padding_body(new_body, sizeof(new_body));
    ret = zmk_runtime_macro_write(name, new_body, new_body_size,
                                  ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_ERR("Slot-resolved write failed: %d", ret);
        return ret;
    }

    uint8_t read_back[sizeof(new_body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read(name, read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != new_body_size || memcmp(read_back, new_body, new_body_size) != 0) {
        LOG_ERR("Slot-resolved read-back mismatch: ret=%d size=%zu", ret, read_size);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_slot_addressing_round_trip");

    /* Unbound (freed) in-range slot: resolving it must fail with -ENOENT,
     * the error the handler maps to "no macro bound to that slot". */
    ret = zmk_runtime_macro_delete(name);
    if (ret < 0) {
        LOG_ERR("Delete before unbound-slot check failed: %d", ret);
        return ret;
    }
    char stale_name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    ret = zmk_runtime_macro_name_for_slot(slot, stale_name, sizeof(stale_name));
    if (ret != -ENOENT) {
        LOG_ERR("Resolving a freed slot returned %d, expected -ENOENT", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_slot_addressing_unbound_slot_rejected");

    /* Out-of-range slot (>= CONFIG_ZMK_RUNTIME_MACRO_COUNT): must fail with
     * -ERANGE, distinctly from the unbound-but-in-range -ENOENT case above -
     * the handler surfaces both as distinct, clear RPC errors. */
    ret = zmk_runtime_macro_name_for_slot(CONFIG_ZMK_RUNTIME_MACRO_COUNT, stale_name,
                                          sizeof(stale_name));
    if (ret != -ERANGE) {
        LOG_ERR("Resolving an out-of-range slot returned %d, expected -ERANGE", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_slot_addressing_out_of_range_rejected");

    return 0;
}

/* has_unsaved_changes (surfaced per-macro in the ListMacros RPC) reports
 * whether a macro has an in-memory-only value not yet written to flash. A
 * MEMORY-mode create is unsaved; a PERSIST-mode create is not. Verified
 * through zmk_runtime_macro_for_each(), the same iteration the list handler
 * uses. */
struct unsaved_probe {
    const char *name;
    bool found;
    bool has_unsaved_changes;
};

static int unsaved_probe_cb(uint32_t slot, const char *name, size_t encoded_size,
                            bool has_unsaved_changes, void *user_data) {
    ARG_UNUSED(slot);
    ARG_UNUSED(encoded_size);
    struct unsaved_probe *probe = user_data;

    if (strcmp(name, probe->name) == 0) {
        probe->found = true;
        probe->has_unsaved_changes = has_unsaved_changes;
    }
    return 0;
}

static int probe_has_unsaved(const char *name, bool *out) {
    struct unsaved_probe probe = {.name = name};
    int ret = zmk_runtime_macro_for_each(unsaved_probe_cb, &probe);
    if (ret < 0 || !probe.found) {
        return ret < 0 ? ret : -ENOENT;
    }
    *out = probe.has_unsaved_changes;
    return 0;
}

static int test_has_unsaved_changes(void) {
    uint8_t body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};

    int ret = zmk_runtime_macro_create("Unsaved Mem", body, sizeof(body),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, NULL);
    if (ret < 0) {
        LOG_ERR("Memory-mode create failed: %d", ret);
        return ret;
    }

    ret = zmk_runtime_macro_create("Saved Flash", body, sizeof(body),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST, NULL);
    if (ret < 0) {
        LOG_ERR("Persist-mode create failed: %d", ret);
        return ret;
    }

    bool mem_unsaved = false;
    bool flash_unsaved = true;
    ret = probe_has_unsaved("Unsaved Mem", &mem_unsaved);
    if (ret < 0) {
        LOG_ERR("Probe of memory-mode macro failed: %d", ret);
        return ret;
    }
    ret = probe_has_unsaved("Saved Flash", &flash_unsaved);
    if (ret < 0) {
        LOG_ERR("Probe of persist-mode macro failed: %d", ret);
        return ret;
    }

    if (!mem_unsaved || flash_unsaved) {
        LOG_ERR("has_unsaved_changes wrong: mem=%d (want 1) flash=%d (want 0)", mem_unsaved,
                flash_unsaved);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_has_unsaved_changes");
    return 0;
}

static int runtime_macro_test_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        LOG_ERR("Failed to init fake settings backend: %d", ret);
        return ret;
    }

    ret = seed_playback_macro();
    if (ret < 0) {
        return ret;
    }

    ret = test_round_trip_large_body();
    if (ret < 0) {
        return ret;
    }

    ret = test_max_bytes_boundary();
    if (ret < 0) {
        return ret;
    }

    ret = test_all_digit_name();
    if (ret < 0) {
        return ret;
    }

    ret = test_name_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_pool_budget_and_discard();
    if (ret < 0) {
        return ret;
    }

    ret = test_tap_ms_persist_discard();
    if (ret < 0) {
        return ret;
    }

    ret = test_slot_addressing();
    if (ret < 0) {
        return ret;
    }

    ret = test_has_unsaved_changes();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

SYS_INIT(runtime_macro_test_init, APPLICATION, 99);
