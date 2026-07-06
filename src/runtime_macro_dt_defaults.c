/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT cormoran_runtime_macro_default

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define RUNTIME_MACRO_DEFAULT_ASSERT_HAS_CONTENT(n)                                                \
    BUILD_ASSERT(DT_NODE_HAS_PROP(n, text) || DT_NODE_HAS_PROP(n, bindings),                       \
                 "cormoran,runtime-macro-default node must set `text` and/or `bindings`");         \
    BUILD_ASSERT(DT_PROP(n, slot) < CONFIG_ZMK_RUNTIME_MACRO_COUNT,                                \
                 "cormoran,runtime-macro-default `slot` is out of range");

DT_FOREACH_STATUS_OKAY(cormoran_runtime_macro_default, RUNTIME_MACRO_DEFAULT_ASSERT_HAS_CONTENT)

/* &macro_tap / &macro_press / &macro_release / &macro_wait_time and &kp are singleton control
 * behaviors from ZMK's behaviors.dtsi. Resolve their device names once, guarded so this file still
 * compiles for keymaps that reference none of them (no `bindings` default at all). */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_tap)
#define RUNTIME_MACRO_DEFAULT_TAP_MODE_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_tap))
#else
#define RUNTIME_MACRO_DEFAULT_TAP_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_press)
#define RUNTIME_MACRO_DEFAULT_PRESS_MODE_NAME                                                      \
    DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_press))
#else
#define RUNTIME_MACRO_DEFAULT_PRESS_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_release)
#define RUNTIME_MACRO_DEFAULT_RELEASE_MODE_NAME                                                    \
    DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_release))
#else
#define RUNTIME_MACRO_DEFAULT_RELEASE_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_wait_time)
#define RUNTIME_MACRO_DEFAULT_WAIT_TIME_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_wait_time))
#else
#define RUNTIME_MACRO_DEFAULT_WAIT_TIME_NAME NULL
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(kp))
#define RUNTIME_MACRO_DEFAULT_KP_NAME DEVICE_DT_NAME(DT_NODELABEL(kp))
#else
#define RUNTIME_MACRO_DEFAULT_KP_NAME NULL
#endif

struct runtime_macro_default_config {
    uint32_t slot;
    const char *display_name;
    const char *text;
    uint32_t wait_ms;
    const struct zmk_behavior_binding *bindings;
    uint32_t bindings_len;
};

#define TRANSFORMED_BEHAVIORS(n)                                                                   \
    {LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n)}

#define RUNTIME_MACRO_DEFAULT_BINDINGS_ARRAY(n)                                                    \
    COND_CODE_1(DT_NODE_HAS_PROP(n, bindings),                                                     \
                (static const struct zmk_behavior_binding runtime_macro_default_bindings_##n[] =   \
                     TRANSFORMED_BEHAVIORS(n);),                                                   \
                ())

#define RUNTIME_MACRO_DEFAULT_INST(n)                                                              \
    RUNTIME_MACRO_DEFAULT_BINDINGS_ARRAY(n)                                                        \
    static const struct runtime_macro_default_config runtime_macro_default_config_##n = {          \
        .slot = DT_PROP(n, slot),                                                                  \
        .display_name = DT_PROP_OR(n, display_name, DT_NODE_FULL_NAME(n)),                         \
        .text = DT_PROP_OR(n, text, NULL),                                                         \
        .wait_ms = DT_PROP_OR(n, wait_ms, 0),                                                      \
        .bindings = COND_CODE_1(DT_NODE_HAS_PROP(n, bindings),                                     \
                                (runtime_macro_default_bindings_##n), (NULL)),                     \
        .bindings_len = DT_PROP_LEN_OR(n, bindings, 0),                                            \
    };

DT_FOREACH_STATUS_OKAY(cormoran_runtime_macro_default, RUNTIME_MACRO_DEFAULT_INST)

#define RUNTIME_MACRO_DEFAULT_CONFIG_PTR(n) &runtime_macro_default_config_##n,

static const struct runtime_macro_default_config *runtime_macro_default_configs[] = {
    DT_FOREACH_STATUS_OKAY(cormoran_runtime_macro_default, RUNTIME_MACRO_DEFAULT_CONFIG_PTR)};

#define RUNTIME_MACRO_DEFAULT_COUNT ARRAY_SIZE(runtime_macro_default_configs)

/* Owned by this file, outlives the settings registry: zmk_custom_setting_set_default() only
 * stores a pointer, it does not copy. */
static struct zmk_custom_setting_value
    runtime_macro_default_body_values[RUNTIME_MACRO_DEFAULT_COUNT];
static struct zmk_custom_setting_value
    runtime_macro_default_name_values[RUNTIME_MACRO_DEFAULT_COUNT];

enum runtime_macro_default_step_mode {
    RUNTIME_MACRO_DEFAULT_MODE_TAP,
    RUNTIME_MACRO_DEFAULT_MODE_PRESS,
    RUNTIME_MACRO_DEFAULT_MODE_RELEASE,
};

struct runtime_macro_encode_state {
    uint8_t *buf;
    size_t capacity;
    size_t size;
    uint8_t pending_packed[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t pending_packed_count;
};

static int emit_byte(struct runtime_macro_encode_state *s, uint8_t byte) {
    if (s->size >= s->capacity) {
        return -ENOSPC;
    }
    s->buf[s->size++] = byte;
    return 0;
}

static int emit_uvar(struct runtime_macro_encode_state *s, uint32_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        int ret = emit_byte(s, value != 0 ? (byte | 0x80) : byte);
        if (ret < 0) {
            return ret;
        }
    } while (value != 0);
    return 0;
}

static int flush_packed_sequence(struct runtime_macro_encode_state *s) {
    if (s->pending_packed_count == 0) {
        return 0;
    }

    int ret = emit_byte(s, ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE);
    if (ret < 0) {
        return ret;
    }
    ret = emit_uvar(s, s->pending_packed_count);
    if (ret < 0) {
        return ret;
    }
    for (size_t i = 0; i < s->pending_packed_count; i++) {
        ret = emit_byte(s, s->pending_packed[i]);
        if (ret < 0) {
            return ret;
        }
    }
    s->pending_packed_count = 0;
    return 0;
}

static int try_append_packed_tap(struct runtime_macro_encode_state *s, uint32_t keycode) {
    if (s->pending_packed_count >= sizeof(s->pending_packed)) {
        return -ENOSPC;
    }

    uint8_t packed;
    int ret = zmk_runtime_macro_pack_key_tap(keycode, &packed);
    if (ret < 0) {
        return ret;
    }

    s->pending_packed[s->pending_packed_count++] = packed;
    return 0;
}

static int emit_delay_step(struct runtime_macro_encode_state *s, uint32_t delay_ms) {
    int ret = flush_packed_sequence(s);
    if (ret < 0) {
        return ret;
    }
    ret = emit_byte(s, ZMK_RUNTIME_MACRO_OP_DELAY);
    if (ret < 0) {
        return ret;
    }
    return emit_uvar(s, delay_ms);
}

static int emit_binding_step(struct runtime_macro_encode_state *s, uint8_t opcode,
                             const struct zmk_behavior_binding *binding) {
    int ret = flush_packed_sequence(s);
    if (ret < 0) {
        return ret;
    }

    zmk_behavior_local_id_t id = zmk_behavior_get_local_id(binding->behavior_dev);
    if (id == UINT16_MAX) {
        LOG_ERR("Runtime macro default: unknown behavior %s", binding->behavior_dev);
        return -ENODEV;
    }

    ret = emit_byte(s, opcode);
    if (ret < 0) {
        return ret;
    }
    ret = emit_uvar(s, id);
    if (ret < 0) {
        return ret;
    }
    ret = emit_uvar(s, binding->param1);
    if (ret < 0) {
        return ret;
    }
    return emit_uvar(s, binding->param2);
}

/* US-layout ASCII -> (HID keyboard usage, left-shift) for the packable range (usages 0x04-0x38),
 * matching what the Web UI's packed key-tap sequence encoding assumes. */
static int ascii_char_to_usage(char c, uint32_t *usage, bool *shift) {
    *shift = false;

    if (c >= 'a' && c <= 'z') {
        *usage = HID_USAGE_KEY_KEYBOARD_A + (c - 'a');
        return 0;
    }
    if (c >= 'A' && c <= 'Z') {
        *usage = HID_USAGE_KEY_KEYBOARD_A + (c - 'A');
        *shift = true;
        return 0;
    }
    if (c >= '1' && c <= '9') {
        *usage = HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION + (c - '1');
        return 0;
    }

    switch (c) {
    case '0':
        *usage = HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
        return 0;
    case '\n':
        *usage = HID_USAGE_KEY_KEYBOARD_RETURN_ENTER;
        return 0;
    case '\t':
        *usage = HID_USAGE_KEY_KEYBOARD_TAB;
        return 0;
    case ' ':
        *usage = HID_USAGE_KEY_KEYBOARD_SPACEBAR;
        return 0;
    case '-':
        *usage = HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
        return 0;
    case '_':
        *usage = HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
        *shift = true;
        return 0;
    case '=':
        *usage = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
        return 0;
    case '+':
        *usage = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
        *shift = true;
        return 0;
    case '[':
        *usage = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
        return 0;
    case '{':
        *usage = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
        *shift = true;
        return 0;
    case ']':
        *usage = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
        return 0;
    case '}':
        *usage = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
        *shift = true;
        return 0;
    case '\\':
        *usage = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
        return 0;
    case '|':
        *usage = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
        *shift = true;
        return 0;
    case ';':
        *usage = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
        return 0;
    case ':':
        *usage = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
        *shift = true;
        return 0;
    case '\'':
        *usage = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
        return 0;
    case '"':
        *usage = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
        *shift = true;
        return 0;
    case '`':
        *usage = HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE;
        return 0;
    case '~':
        *usage = HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE;
        *shift = true;
        return 0;
    case ',':
        *usage = HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
        return 0;
    case '<':
        *usage = HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
        *shift = true;
        return 0;
    case '.':
        *usage = HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
        return 0;
    case '>':
        *usage = HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
        *shift = true;
        return 0;
    case '/':
        *usage = HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
        return 0;
    case '?':
        *usage = HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
        *shift = true;
        return 0;
    case '!':
        *usage = HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION;
        *shift = true;
        return 0;
    case '@':
        *usage = HID_USAGE_KEY_KEYBOARD_2_AND_AT;
        *shift = true;
        return 0;
    case '#':
        *usage = HID_USAGE_KEY_KEYBOARD_3_AND_HASH;
        *shift = true;
        return 0;
    case '$':
        *usage = HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR;
        *shift = true;
        return 0;
    case '%':
        *usage = HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT;
        *shift = true;
        return 0;
    case '^':
        *usage = HID_USAGE_KEY_KEYBOARD_6_AND_CARET;
        *shift = true;
        return 0;
    case '&':
        *usage = HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND;
        *shift = true;
        return 0;
    case '*':
        *usage = HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK;
        *shift = true;
        return 0;
    case '(':
        *usage = HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS;
        *shift = true;
        return 0;
    case ')':
        *usage = HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
        *shift = true;
        return 0;
    default:
        return -EINVAL;
    }
}

static int encode_text(struct runtime_macro_encode_state *s, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        uint32_t usage;
        bool shift;
        int ret = ascii_char_to_usage(*p, &usage, &shift);
        if (ret < 0) {
            LOG_ERR("Runtime macro default: unsupported character 0x%02x in `text`",
                    (unsigned)(unsigned char)*p);
            return ret;
        }

        uint32_t keycode = ZMK_HID_USAGE(HID_USAGE_KEY, usage);
        if (shift) {
            keycode = LS(keycode);
        }

        ret = try_append_packed_tap(s, keycode);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static bool name_matches(const char *name, const char *control_name) {
    return control_name != NULL && name != NULL && strcmp(name, control_name) == 0;
}

static int encode_bindings(struct runtime_macro_encode_state *s,
                           const struct runtime_macro_default_config *cfg) {
    enum runtime_macro_default_step_mode mode = RUNTIME_MACRO_DEFAULT_MODE_TAP;
    bool need_delay_before_next = false;

    for (uint32_t i = 0; i < cfg->bindings_len; i++) {
        const struct zmk_behavior_binding *binding = &cfg->bindings[i];

        if (name_matches(binding->behavior_dev, RUNTIME_MACRO_DEFAULT_TAP_MODE_NAME)) {
            mode = RUNTIME_MACRO_DEFAULT_MODE_TAP;
            continue;
        }
        if (name_matches(binding->behavior_dev, RUNTIME_MACRO_DEFAULT_PRESS_MODE_NAME)) {
            mode = RUNTIME_MACRO_DEFAULT_MODE_PRESS;
            continue;
        }
        if (name_matches(binding->behavior_dev, RUNTIME_MACRO_DEFAULT_RELEASE_MODE_NAME)) {
            mode = RUNTIME_MACRO_DEFAULT_MODE_RELEASE;
            continue;
        }
        if (name_matches(binding->behavior_dev, RUNTIME_MACRO_DEFAULT_WAIT_TIME_NAME)) {
            int ret = emit_delay_step(s, binding->param1);
            if (ret < 0) {
                return ret;
            }
            need_delay_before_next = false;
            continue;
        }

        if (mode == RUNTIME_MACRO_DEFAULT_MODE_TAP &&
            name_matches(binding->behavior_dev, RUNTIME_MACRO_DEFAULT_KP_NAME) &&
            try_append_packed_tap(s, binding->param1) == 0) {
            need_delay_before_next = false;
            continue;
        }

        if (need_delay_before_next && cfg->wait_ms > 0) {
            int ret = emit_delay_step(s, cfg->wait_ms);
            if (ret < 0) {
                return ret;
            }
        }

        uint8_t opcode;
        switch (mode) {
        case RUNTIME_MACRO_DEFAULT_MODE_PRESS:
            opcode = ZMK_RUNTIME_MACRO_OP_DOWN;
            break;
        case RUNTIME_MACRO_DEFAULT_MODE_RELEASE:
            opcode = ZMK_RUNTIME_MACRO_OP_UP;
            break;
        default:
            opcode = ZMK_RUNTIME_MACRO_OP_TAP;
            break;
        }

        int ret = emit_binding_step(s, opcode, binding);
        if (ret < 0) {
            return ret;
        }
        need_delay_before_next = true;
    }

    return 0;
}

static int install_one_default(size_t storage_idx, const struct runtime_macro_default_config *cfg,
                               bool *slot_used) {
    if (slot_used[cfg->slot]) {
        LOG_ERR("Runtime macro default: slot %u already has a default, ignoring duplicate",
                cfg->slot);
        return -EALREADY;
    }

    /* v1 limitation (see docs/design/large-macros-shared-pool.md §B.5):
     * zmk_custom_setting_set_default() only accepts the 64-byte carrier value
     * even for pooled/large-capable settings, so a DT-provided default body
     * must stay within CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE (64) encoded
     * bytes even though a runtime-written macro may use the full
     * CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES. An upstream `set_default_bytes`
     * variant (pointer + size) would lift this. */
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    struct runtime_macro_encode_state state = {
        .buf = encoded,
        .capacity = sizeof(encoded),
    };

    int ret = emit_byte(&state, ZMK_RUNTIME_MACRO_FORMAT_VERSION);
    if (ret < 0) {
        goto encode_failed;
    }

    if (cfg->text) {
        ret = encode_text(&state, cfg->text);
        if (ret < 0) {
            goto encode_failed;
        }
    }

    if (cfg->bindings_len > 0) {
        ret = encode_bindings(&state, cfg);
        if (ret < 0) {
            goto encode_failed;
        }
    }

    ret = flush_packed_sequence(&state);
    if (ret < 0) {
        goto encode_failed;
    }

    ret = zmk_runtime_macro_validate_encoded(state.buf, state.size);
    if (ret < 0) {
        goto encode_failed;
    }

    /* Names/bodies are plain per-slot scalars keyed "<prefix>.<slot>" (not P3
     * array elements - see src/runtime_macro.c and the '.' vs '/' separator
     * note on ZMK_RUNTIME_MACRO_NAMES_KEY/BODIES_KEY in the header), so slots
     * are looked up by formatted key rather than by array index. `cfg->slot`
     * is a devicetree-derived runtime value, so this can't be resolved at
     * compile time the way src/runtime_macro.c's descriptor tables are. */
    char body_key[sizeof(ZMK_RUNTIME_MACRO_BODIES_KEY) + 10];
    char name_key[sizeof(ZMK_RUNTIME_MACRO_NAMES_KEY) + 10];
    snprintf(body_key, sizeof(body_key), ZMK_RUNTIME_MACRO_BODIES_KEY ".%u", cfg->slot);
    snprintf(name_key, sizeof(name_key), ZMK_RUNTIME_MACRO_NAMES_KEY ".%u", cfg->slot);

    const struct zmk_custom_setting *body_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, body_key);
    const struct zmk_custom_setting *name_setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, name_key);
    if (!body_setting || !name_setting) {
        LOG_ERR("Runtime macro default: slot %u setting not registered", cfg->slot);
        return -ENODEV;
    }

    struct zmk_custom_setting_value *body_value = &runtime_macro_default_body_values[storage_idx];
    body_value->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES;
    body_value->size = state.size;
    memcpy(body_value->bytes_value, state.buf, state.size);

    struct zmk_custom_setting_value *name_value = &runtime_macro_default_name_values[storage_idx];
    name_value->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
    size_t name_len = strlen(cfg->display_name);
    name_value->size = MIN(name_len, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    memcpy(name_value->string_value, cfg->display_name, name_value->size);
    name_value->string_value[name_value->size] = '\0';

    ret = zmk_custom_setting_set_default(body_setting, body_value);
    if (ret < 0) {
        LOG_ERR("Runtime macro default: slot %u body default rejected: %d", cfg->slot, ret);
        return ret;
    }
    ret = zmk_custom_setting_set_default(name_setting, name_value);
    if (ret < 0) {
        LOG_ERR("Runtime macro default: slot %u name default rejected: %d", cfg->slot, ret);
        return ret;
    }

    slot_used[cfg->slot] = true;
    return 0;

encode_failed:
    if (ret == -ENOSPC) {
        LOG_ERR("Runtime macro default: slot %u DT default exceeds the %u-byte default limit "
                "(runtime-written macros may be larger, up to "
                "CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES=%u); skipping",
                cfg->slot, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES);
    } else {
        LOG_ERR("Runtime macro default: slot %u failed to encode: %d", cfg->slot, ret);
    }
    return ret;
}

static int runtime_macro_install_dt_defaults(void) {
    bool slot_used[CONFIG_ZMK_RUNTIME_MACRO_COUNT] = {0};

    for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {
        /* Best-effort: one bad default must not prevent the rest from installing. */
        install_one_default(i, runtime_macro_default_configs[i], slot_used);
    }

    return 0;
}

/* Must run after behavior_local_id_init() (zmk/src/behavior.c, APPLICATION/
 * CONFIG_APPLICATION_INIT_PRIORITY, 90 by default) so behavior local IDs resolve, and before
 * settings_load() (always later, from main()). zmk_custom_setting_set_default() itself is safe
 * regardless of ordering relative to custom_settings_init() (same level/priority), so only
 * running after behavior_local_id_init matters here. SYS_INIT priority must be a plain literal
 * (it is stringified into a linker section name), so this can't be expressed as
 * CONFIG_APPLICATION_INIT_PRIORITY + 1 - use a fixed priority safely above the default instead. */
SYS_INIT(runtime_macro_install_dt_defaults, APPLICATION, 91);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
