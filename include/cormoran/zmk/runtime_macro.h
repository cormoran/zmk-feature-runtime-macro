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
/* Key *prefixes*: each slot `i` is stored under "<prefix>/<i>" (e.g. "names/0",
 * "macros/0") as its own scalar custom setting - see src/runtime_macro.c. */
#define ZMK_RUNTIME_MACRO_NAMES_KEY "names"
#define ZMK_RUNTIME_MACRO_BODIES_KEY "macros"
#define ZMK_RUNTIME_MACRO_TAP_MS_KEY "tap_ms"

#define ZMK_RUNTIME_MACRO_FORMAT_VERSION 1U

enum zmk_runtime_macro_opcode {
    ZMK_RUNTIME_MACRO_OP_DOWN = 1,
    ZMK_RUNTIME_MACRO_OP_UP = 2,
    ZMK_RUNTIME_MACRO_OP_TAP = 3,
    ZMK_RUNTIME_MACRO_OP_DELAY = 4,
    ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE = 5,
};

int zmk_runtime_macro_pack_key_tap(uint32_t keycode, uint8_t *packed_key);
int zmk_runtime_macro_unpack_key_tap(uint8_t packed_key, uint32_t *keycode);
int zmk_runtime_macro_validate_encoded(const uint8_t *encoded, size_t size);
int zmk_runtime_macro_play(uint32_t index, const struct zmk_behavior_binding_event *event);
int zmk_runtime_macro_read(uint32_t index, char *name, size_t name_capacity, uint8_t *encoded,
                           size_t encoded_capacity, size_t *encoded_size);
int zmk_runtime_macro_write(uint32_t index, const char *name, const uint8_t *encoded,
                            size_t encoded_size, bool persist);

/* Total bytes configured for the shared macro-body pool
 * (CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES) and the bytes currently occupied by
 * all slots' bodies combined - for RPC/UI budget reporting. */
size_t zmk_runtime_macro_pool_total(void);
size_t zmk_runtime_macro_pool_used(void);
