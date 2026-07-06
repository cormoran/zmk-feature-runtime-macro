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

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Exercises the slot 0 devicetree default (`text = "hi"`, `display-name = "Hi"` - see
 * tests/dt-defaults/native_sim.keymap) through its full lifecycle: installed with no RPC/write
 * ever happening, shadowed by a user write, then restored by reset(). Slot 1's `bindings`-based
 * default is instead verified end-to-end through actual key playback (see
 * tests/dt-defaults/keycode_events.snapshot). */
static int runtime_macro_dt_defaults_test_init(void) {
    char name[32];
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t encoded_size;

    int ret =
        zmk_runtime_macro_read(0, name, sizeof(name), encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        LOG_ERR("Failed to read runtime macro default: %d", ret);
        return ret;
    }
    if (strcmp(name, "Hi") != 0 || encoded_size == 0) {
        LOG_ERR("Unexpected devicetree default name=%s size=%zu", name, encoded_size);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_installed name=%s size=%u", name,
            (unsigned)encoded_size);

    static const uint8_t override_body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION,
                                            ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE, 1, 0x05};
    ret = zmk_runtime_macro_write(0, "Overridden", override_body, sizeof(override_body), false);
    if (ret < 0) {
        LOG_ERR("Failed to write runtime macro override: %d", ret);
        return ret;
    }
    ret = zmk_runtime_macro_read(0, name, sizeof(name), encoded, sizeof(encoded), &encoded_size);
    if (ret < 0 || strcmp(name, "Overridden") != 0) {
        LOG_ERR("Override write did not take effect: ret=%d name=%s", ret, name);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_overridden name=%s", name);

    /* Slot 0's name/body are plain per-slot scalars keyed "<prefix>.0" (not P3
     * array elements) - see src/runtime_macro.c. */
    const struct zmk_custom_setting *name_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_NAMES_KEY ".0");
    const struct zmk_custom_setting *body_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_BODIES_KEY ".0");
    if (!name_setting || !body_setting) {
        LOG_ERR("Runtime macro slot 0 settings not found");
        return -ENODEV;
    }
    ret = zmk_custom_setting_reset(name_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(body_setting);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_runtime_macro_read(0, name, sizeof(name), encoded, sizeof(encoded), &encoded_size);
    if (ret < 0 || strcmp(name, "Hi") != 0) {
        LOG_ERR("Reset did not restore devicetree default: ret=%d name=%s", ret, name);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_restored_after_reset name=%s", name);

    /* Slot 2's devicetree default (`text` of 72 'a's, see
     * tests/dt-defaults/native_sim.keymap) encodes to well over the 64-byte
     * carrier zmk_custom_setting_set_default() accepts (v1 limitation - see
     * docs/design/large-macros-shared-pool.md §B.5). install_one_default()
     * must skip it with a clear log rather than crash or corrupt the slot,
     * and - since it's best-effort - must not have prevented slots 0/1's
     * defaults (already verified above) from installing. A skipped default
     * leaves the slot at its compiled-in empty default. */
    ret = zmk_runtime_macro_read(2, name, sizeof(name), encoded, sizeof(encoded), &encoded_size);
    if (ret < 0 || name[0] != '\0' || encoded_size != 0) {
        LOG_ERR("Oversized devicetree default was not cleanly skipped: ret=%d name=%s size=%zu",
                ret, name, encoded_size);
        return -EINVAL;
    }
    LOG_INF("PASS: runtime_macro_dt_default_oversized_skipped");

    return 0;
}

SYS_INIT(runtime_macro_dt_defaults_test_init, APPLICATION, 99);
