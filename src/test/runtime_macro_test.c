/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <cormoran/zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int runtime_macro_test_init(void) {
    const uint8_t encoded[] = {
        ZMK_RUNTIME_MACRO_FORMAT_VERSION,
        ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE,
        1,
        0x04,
    };

    int ret = zmk_runtime_macro_write(0, "Test A", encoded, sizeof(encoded), false);
    if (ret < 0) {
        LOG_ERR("Failed to seed runtime macro test data: %d", ret);
        return ret;
    }

    return 0;
}

SYS_INIT(runtime_macro_test_init, APPLICATION, 99);
