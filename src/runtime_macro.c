/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct runtime_macro_queue_item {
    bool is_delay;
    bool use_tap_ms;
    bool pressed;
    uint32_t delay_ms;
    struct zmk_behavior_binding binding;
};

struct runtime_macro_player {
    struct k_work_delayable work;
    struct k_mutex lock;
    bool active;
    size_t cursor;
    size_t count;
    struct zmk_behavior_binding_event event;
    struct runtime_macro_queue_item items[CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE];
};

static void runtime_macro_work_handler(struct k_work *work);

static struct runtime_macro_player player = {
    .work = Z_WORK_DELAYABLE_INITIALIZER(runtime_macro_work_handler),
    .lock = Z_MUTEX_INITIALIZER(player.lock),
};

#define DEFINE_RUNTIME_MACRO_NAME_SETTING(i, _)                                                    \
    ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(                                                       \
        runtime_macro_name_##i, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_NAMES_KEY, i,    \
        CONFIG_ZMK_RUNTIME_MACRO_COUNT, ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                      \
        ZMK_CUSTOM_SETTING_VALUE_STRING(""), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,        \
        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,            \
        ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

#define DEFINE_RUNTIME_MACRO_BODY_SETTING(i, _)                                                    \
    ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(                                                       \
        runtime_macro_body_##i, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_BODIES_KEY, i,   \
        CONFIG_ZMK_RUNTIME_MACRO_COUNT, ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                       \
        ZMK_CUSTOM_SETTING_VALUE_BYTES(), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,           \
        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,            \
        ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

LISTIFY(CONFIG_ZMK_RUNTIME_MACRO_COUNT, DEFINE_RUNTIME_MACRO_NAME_SETTING, (), _)
LISTIFY(CONFIG_ZMK_RUNTIME_MACRO_COUNT, DEFINE_RUNTIME_MACRO_BODY_SETTING, (), _)

ZMK_CUSTOM_SETTING_DEFINE(runtime_macro_tap_ms, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                          ZMK_RUNTIME_MACRO_TAP_MS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_RUNTIME_MACRO_DEFAULT_TAP_MS),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static int read_uvar(const uint8_t *encoded, size_t size, size_t *offset, uint32_t *value) {
    uint32_t result = 0;
    uint8_t shift = 0;

    while (*offset < size && shift <= 28) {
        uint8_t byte = encoded[(*offset)++];
        result |= (uint32_t)(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            *value = result;
            return 0;
        }

        shift += 7;
    }

    return -EINVAL;
}

static int read_binding(const uint8_t *encoded, size_t size, size_t *offset,
                        struct zmk_behavior_binding *binding) {
    uint32_t behavior_id;
    int ret = read_uvar(encoded, size, offset, &behavior_id);
    if (ret < 0) {
        return ret;
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id((zmk_behavior_local_id_t)behavior_id);
    if (!behavior_name) {
        return -ENODEV;
    }

    uint32_t param1;
    uint32_t param2;
    ret = read_uvar(encoded, size, offset, &param1);
    if (ret < 0) {
        return ret;
    }
    ret = read_uvar(encoded, size, offset, &param2);
    if (ret < 0) {
        return ret;
    }

    *binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = param1,
        .param2 = param2,
    };

    return zmk_behavior_validate_binding(binding);
}

static int append_item(struct runtime_macro_queue_item *items, size_t *count,
                       struct runtime_macro_queue_item item) {
    if (*count >= CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE) {
        return -ENOSPC;
    }

    items[(*count)++] = item;
    return 0;
}

static int decode_macro(const uint8_t *encoded, size_t size, struct runtime_macro_queue_item *items,
                        size_t *count) {
    *count = 0;

    if (size == 0) {
        return 0;
    }

    if (encoded[0] != ZMK_RUNTIME_MACRO_FORMAT_VERSION) {
        return -EINVAL;
    }

    size_t offset = 1;

    while (offset < size) {
        uint8_t opcode = encoded[offset++];

        if (opcode == ZMK_RUNTIME_MACRO_OP_DELAY) {
            uint32_t delay_ms;
            int ret = read_uvar(encoded, size, &offset, &delay_ms);
            if (ret < 0) {
                return ret;
            }

            ret = append_item(items, count,
                              (struct runtime_macro_queue_item){
                                  .is_delay = true,
                                  .delay_ms = delay_ms,
                              });
            if (ret < 0) {
                return ret;
            }
            continue;
        }

        struct zmk_behavior_binding binding;
        int ret = read_binding(encoded, size, &offset, &binding);
        if (ret < 0) {
            return ret;
        }

        switch (opcode) {
        case ZMK_RUNTIME_MACRO_OP_DOWN:
            ret = append_item(items, count,
                              (struct runtime_macro_queue_item){
                                  .binding = binding,
                                  .pressed = true,
                              });
            break;
        case ZMK_RUNTIME_MACRO_OP_UP:
            ret = append_item(items, count,
                              (struct runtime_macro_queue_item){
                                  .binding = binding,
                                  .pressed = false,
                              });
            break;
        case ZMK_RUNTIME_MACRO_OP_TAP:
            ret = append_item(items, count,
                              (struct runtime_macro_queue_item){
                                  .binding = binding,
                                  .pressed = true,
                              });
            ret = append_item(items, count,
                              (struct runtime_macro_queue_item){
                                  .is_delay = true,
                                  .use_tap_ms = true,
                              });
            if (ret == 0) {
                ret = append_item(items, count,
                                  (struct runtime_macro_queue_item){
                                      .binding = binding,
                                      .pressed = false,
                                  });
            }
            break;
        default:
            return -EINVAL;
        }

        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static uint32_t get_runtime_macro_tap_ms(void) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_MACRO_TAP_MS_KEY, &value);
    if (ret < 0 || value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value < 0) {
        return CONFIG_ZMK_RUNTIME_MACRO_DEFAULT_TAP_MS;
    }

    return MIN(value.int32_value, 10000);
}

int zmk_runtime_macro_validate_encoded(const uint8_t *encoded, size_t size) {
    struct runtime_macro_queue_item items[CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE];
    size_t count;

    return decode_macro(encoded, size, items, &count);
}

int zmk_runtime_macro_read(uint32_t index, char *name, size_t name_capacity, uint8_t *encoded,
                           size_t encoded_capacity, size_t *encoded_size) {
    if (index >= CONFIG_ZMK_RUNTIME_MACRO_COUNT) {
        return -ERANGE;
    }

    struct zmk_custom_setting_value name_value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_MACRO_NAMES_KEY, index, &name_value);
    if (ret < 0) {
        return ret;
    }

    if (name && name_capacity > 0) {
        size_t copy_size = MIN(name_value.size, name_capacity - 1);
        memcpy(name, name_value.string_value, copy_size);
        name[copy_size] = '\0';
    }

    struct zmk_custom_setting_value body_value;
    ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                               ZMK_RUNTIME_MACRO_BODIES_KEY, index, &body_value);
    if (ret < 0) {
        return ret;
    }

    if (body_value.size > encoded_capacity) {
        return -EMSGSIZE;
    }

    if (encoded && body_value.size > 0) {
        memcpy(encoded, body_value.bytes_value, body_value.size);
    }
    if (encoded_size) {
        *encoded_size = body_value.size;
    }

    return 0;
}

int zmk_runtime_macro_write(uint32_t index, const char *name, const uint8_t *encoded,
                            size_t encoded_size, bool persist) {
    if (index >= CONFIG_ZMK_RUNTIME_MACRO_COUNT) {
        return -ERANGE;
    }

    int ret = zmk_runtime_macro_validate_encoded(encoded, encoded_size);
    if (ret < 0) {
        return ret;
    }

    enum zmk_custom_setting_write_mode mode =
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    struct zmk_custom_setting_value name_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    };
    if (name) {
        name_value.size = MIN(strlen(name), CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        memcpy(name_value.string_value, name, name_value.size);
        name_value.string_value[name_value.size] = '\0';
    }

    ret = zmk_custom_setting_write_array_by_key(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_NAMES_KEY, index, &name_value, mode);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value body_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = encoded_size,
    };
    if (encoded_size > 0) {
        memcpy(body_value.bytes_value, encoded, encoded_size);
    }

    return zmk_custom_setting_write_array_by_key(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_BODIES_KEY, index, &body_value, mode);
}

static void runtime_macro_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    while (true) {
        k_mutex_lock(&player.lock, K_FOREVER);

        if (player.cursor >= player.count) {
            player.active = false;
            k_mutex_unlock(&player.lock);
            return;
        }

        struct runtime_macro_queue_item item = player.items[player.cursor++];
        struct zmk_behavior_binding_event event = player.event;
        event.timestamp = k_uptime_get();

        if (item.is_delay) {
            if (item.use_tap_ms) {
                item.delay_ms = get_runtime_macro_tap_ms();
            }
            k_mutex_unlock(&player.lock);
            k_work_schedule(&player.work, K_MSEC(item.delay_ms));
            return;
        }

        k_mutex_unlock(&player.lock);

        int ret = zmk_behavior_invoke_binding(&item.binding, event, item.pressed);
        if (ret < 0) {
            LOG_WRN("Runtime macro behavior invocation failed: %d", ret);
        }
    }
}

int zmk_runtime_macro_play(uint32_t index, const struct zmk_behavior_binding_event *event) {
    if (index >= CONFIG_ZMK_RUNTIME_MACRO_COUNT) {
        return -ERANGE;
    }

    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t encoded_size = 0;

    int ret = zmk_runtime_macro_read(index, NULL, 0, encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    struct runtime_macro_queue_item items[CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE];
    size_t count = 0;
    ret = decode_macro(encoded, encoded_size, items, &count);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&player.lock, K_FOREVER);
    if (player.active) {
        k_mutex_unlock(&player.lock);
        return -EBUSY;
    }

    memcpy(player.items, items, sizeof(items[0]) * count);
    player.count = count;
    player.cursor = 0;
    player.event = *event;
    player.active = true;
    k_mutex_unlock(&player.lock);

    k_work_schedule(&player.work, K_NO_WAIT);

    return 0;
}
