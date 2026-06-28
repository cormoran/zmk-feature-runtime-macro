/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/keys.h>
#include <cormoran/zmk/runtime_macro.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int append_uvar(uint8_t *dest, size_t capacity, size_t *offset, uint32_t value) {
    do {
        if (*offset >= capacity) {
            return -ENOSPC;
        }

        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        dest[(*offset)++] = byte;
    } while (value != 0);

    return 0;
}

static int runtime_macro_test_init(void) {
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = {
        ZMK_RUNTIME_MACRO_FORMAT_VERSION,
        ZMK_RUNTIME_MACRO_OP_TAP,
    };
    size_t size = 2;
    zmk_behavior_local_id_t kp_id = zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp)));

    int ret = append_uvar(encoded, sizeof(encoded), &size, kp_id);
    if (ret == 0) {
        ret = append_uvar(encoded, sizeof(encoded), &size, A);
    }
    if (ret == 0) {
        ret = append_uvar(encoded, sizeof(encoded), &size, 0);
    }
    if (ret < 0) {
        return ret;
    }

    ret = zmk_runtime_macro_write(0, "Test A", encoded, size, false);
    if (ret < 0) {
        LOG_ERR("Failed to seed runtime macro test data: %d", ret);
        return ret;
    }

    return 0;
}

SYS_INIT(runtime_macro_test_init, APPLICATION, 99);
