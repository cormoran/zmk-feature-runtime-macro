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
 * test below exercise a real store/reload round trip instead of only RAM
 * state.
 */
#define TEST_SETTINGS_STORAGE_CAPACITY 16

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
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
 * at CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES's default of 256. Unlike a packed key
 * sequence (1 byte == 1 queue item, so bodies over ~2x QUEUE_SIZE bytes can
 * never validate), a DELAY op's 2-6 byte cost per queue item lets a body
 * exceed the old 64-byte settings carrier - and reach the new, much larger
 * MAX_BYTES ceiling - while decoding to only a handful of items.
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

/* Slot 0 doubles as the existing playback test (see tests/test/native_sim.keymap's
 * `&rmacro 0` binding and tests/test/keycode_events.snapshot): its body is now
 * intentionally > 64 bytes (the old settings-carrier limit) to exercise
 * playback of a macro that only fits through the shared pool / read_into path
 * added by this feature. 62 identical packed key taps of usage 0x04 ('a')
 * encode to 65 bytes (1 + 1 + 1 + 62) and decode to 62 queued taps - well
 * under CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE's default of 64. */
#define PLAYBACK_TEST_KEY_COUNT 62

static int seed_playback_macro(void) {
    uint8_t encoded[3 + PLAYBACK_TEST_KEY_COUNT];

    encoded[0] = ZMK_RUNTIME_MACRO_FORMAT_VERSION;
    encoded[1] = ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE;
    encoded[2] = PLAYBACK_TEST_KEY_COUNT;
    memset(&encoded[3], 0x04, PLAYBACK_TEST_KEY_COUNT);

    BUILD_ASSERT(sizeof(encoded) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
                 "Playback test body must exceed the old 64-byte carrier limit");

    int ret = zmk_runtime_macro_write(0, "Long Macro", encoded, sizeof(encoded), false);
    if (ret < 0) {
        LOG_ERR("Failed to seed runtime macro playback test data: %d", ret);
        return ret;
    }

    return 0;
}

/* Round-trips a body well above the old 64-byte settings carrier through
 * write -> read on a scratch slot, verifying both size and content. */
static int test_round_trip_large_body(void) {
    uint8_t body[150];
    size_t body_size = build_delay_padding_body(body, sizeof(body));

    int ret = zmk_runtime_macro_write(2, "Round Trip", body, body_size, false);
    if (ret < 0) {
        LOG_ERR("Round-trip write failed: %d", ret);
        return ret;
    }

    char name[32];
    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read(2, name, sizeof(name), read_back, sizeof(read_back), &read_size);
    if (ret < 0) {
        LOG_ERR("Round-trip read failed: %d", ret);
        return ret;
    }

    if (read_size != body_size || memcmp(read_back, body, body_size) != 0 ||
        strcmp(name, "Round Trip") != 0) {
        LOG_ERR("Round-trip mismatch: size=%zu name=%s", read_size, name);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_round_trip_large_body size=%u", (unsigned)read_size);
    return 0;
}

/* Exactly CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES must be accepted; one byte more
 * must fail with -EMSGSIZE and must not disturb the slot's stored value. */
static int test_max_bytes_boundary(void) {
    uint8_t at_max[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t at_max_size = build_delay_padding_body(at_max, sizeof(at_max));

    int ret = zmk_runtime_macro_write(3, "At Max", at_max, at_max_size, false);
    if (ret < 0) {
        LOG_ERR("Write at exactly MAX_BYTES failed: %d", ret);
        return ret;
    }
    LOG_INF("PASS: runtime_macro_max_bytes_boundary_ok size=%u", (unsigned)at_max_size);

    uint8_t over_max[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES + 1];
    size_t over_max_size = build_delay_padding_body(over_max, sizeof(over_max));

    ret = zmk_runtime_macro_write(3, "Over Max", over_max, over_max_size, false);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Write over MAX_BYTES returned %d, expected -EMSGSIZE", ret);
        return -EINVAL;
    }

    /* The rejected write must not have touched the previously stored value. */
    char name[32];
    uint8_t read_back[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read(3, name, sizeof(name), read_back, sizeof(read_back), &read_size);
    if (ret < 0 || read_size != at_max_size || strcmp(name, "At Max") != 0) {
        LOG_ERR("Slot 3 changed after a rejected oversized write: ret=%d size=%zu name=%s", ret,
                read_size, name);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_max_bytes_boundary_rejected");
    return 0;
}

/* Fills the shared pool (slots 4..COUNT-1) with MAX_BYTES bodies until a
 * write fails with -ENOSPC, then frees one slot and confirms the retry
 * succeeds - and, in the same step, exercises persist -> memory-mode
 * overwrite -> discard on that large body (the pooled-value analogue of the
 * existing scalar DT-default reset test in runtime_macro_dt_defaults_test.c).
 */
static int test_pool_budget_and_discard(void) {
    const uint32_t scratch_start = 4;
    const uint32_t scratch_count = CONFIG_ZMK_RUNTIME_MACRO_COUNT - scratch_start;

    BUILD_ASSERT(CONFIG_ZMK_RUNTIME_MACRO_COUNT > 4,
                 "Pool budget test needs scratch slots beyond 0-3");

    uint8_t body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t body_size = build_delay_padding_body(body, sizeof(body));

    int first_success_slot = -1;
    int enospc_slot = -1;

    for (uint32_t i = 0; i < scratch_count; i++) {
        uint32_t slot = scratch_start + i;
        int ret = zmk_runtime_macro_write(slot, "Pool Fill", body, body_size, false);
        if (ret == -ENOSPC) {
            enospc_slot = (int)slot;
            break;
        }
        if (ret < 0) {
            LOG_ERR("Unexpected error filling slot %u: %d", slot, ret);
            return ret;
        }
        if (first_success_slot < 0) {
            first_success_slot = (int)slot;
        }
    }

    if (enospc_slot < 0 || first_success_slot < 0) {
        LOG_ERR("Pool budget test did not observe -ENOSPC within available scratch slots "
                "(first_success=%d, pool_used=%zu/%zu) - is CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES "
                "large enough to hold every scratch slot?",
                first_success_slot, zmk_runtime_macro_pool_used(), zmk_runtime_macro_pool_total());
        return -EINVAL;
    }
    /* enospc_slot/first_success_slot depend on however much of the pool the
     * earlier tests (and any DT defaults) already consumed, so they are
     * logged at DBG (not asserted by the snapshot-based test harness, which
     * needs deterministic output) rather than folded into the PASS line. */
    LOG_DBG("Pool exhausted at slot %d after slot %d succeeded", enospc_slot, first_success_slot);
    LOG_INF("PASS: runtime_macro_pool_exhausted");

    /* Free the first slot that succeeded and retry the one that hit -ENOSPC. */
    int ret = zmk_runtime_macro_write((uint32_t)first_success_slot, "", NULL, 0, false);
    if (ret < 0) {
        LOG_ERR("Failed to free slot %d: %d", first_success_slot, ret);
        return ret;
    }

    ret = zmk_runtime_macro_write((uint32_t)enospc_slot, "Pool Test", body, body_size, true);
    if (ret < 0) {
        LOG_ERR("Retry write after freeing a slot failed: %d", ret);
        return ret;
    }
    LOG_INF("PASS: runtime_macro_pool_freed_slot_retry_succeeded");

    /* Persist -> overwrite in memory mode -> discard restores the persisted
     * (> 64 byte) body, exercising the pooled-body analogue of custom-settings'
     * discard/ensure_region path. */
    char persisted_key[16];
    snprintf(persisted_key, sizeof(persisted_key), "macros.%d", enospc_slot);
    const struct zmk_custom_setting *body_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, persisted_key);
    if (!body_setting) {
        LOG_ERR("Could not find body setting for slot %d", enospc_slot);
        return -ENODEV;
    }

    /* A trivially valid (empty-effect) body: just the format version byte. */
    uint8_t overwrite_body[1] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};
    size_t overwrite_size = sizeof(overwrite_body);

    ret = zmk_runtime_macro_write((uint32_t)enospc_slot, "Pool Test", overwrite_body,
                                  overwrite_size, false);
    if (ret < 0) {
        LOG_ERR("Memory-mode overwrite before discard failed: %d", ret);
        return ret;
    }

    ret = zmk_custom_setting_discard(body_setting);
    if (ret < 0) {
        LOG_ERR("Discard of pooled body setting failed: %d", ret);
        return ret;
    }

    char name[32];
    uint8_t read_back[sizeof(body)];
    size_t read_size = 0;
    ret = zmk_runtime_macro_read((uint32_t)enospc_slot, name, sizeof(name), read_back,
                                 sizeof(read_back), &read_size);
    if (ret < 0 || read_size != body_size || memcmp(read_back, body, body_size) != 0) {
        LOG_ERR("Discard did not restore the persisted large body: ret=%d size=%zu", ret,
                read_size);
        return -EINVAL;
    }

    LOG_INF("PASS: runtime_macro_pool_discard_restores_persisted_body size=%u",
            (unsigned)read_size);
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

    ret = test_pool_budget_and_discard();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

SYS_INIT(runtime_macro_test_init, APPLICATION, 99);
