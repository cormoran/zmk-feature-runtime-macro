/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define ZMK_RUNTIME_MACRO_SUBSYSTEM_ID "cormoran__runtime_macro"
#define ZMK_RUNTIME_MACRO_NAMES_KEY "names"
#define ZMK_RUNTIME_MACRO_BODIES_KEY "macros"
#define ZMK_RUNTIME_MACRO_TAP_MS_KEY "tap_ms"

#define ZMK_RUNTIME_MACRO_FORMAT_VERSION 1U

enum zmk_runtime_macro_opcode {
    ZMK_RUNTIME_MACRO_OP_DOWN = 1,
    ZMK_RUNTIME_MACRO_OP_UP = 2,
    ZMK_RUNTIME_MACRO_OP_TAP = 3,
    ZMK_RUNTIME_MACRO_OP_DELAY = 4,
};

int zmk_runtime_macro_validate_encoded(const uint8_t *encoded, size_t size);
int zmk_runtime_macro_play(uint32_t index, const struct zmk_behavior_binding_event *event);
int zmk_runtime_macro_read(uint32_t index, char *name, size_t name_capacity, uint8_t *encoded,
                           size_t encoded_capacity, size_t *encoded_size);
int zmk_runtime_macro_write(uint32_t index, const char *name, const uint8_t *encoded,
                            size_t encoded_size, bool persist);
