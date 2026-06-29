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

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RUNTIME_MACRO_WAIT_BEHAVIOR_NAME "runtime_macro_wait"

struct runtime_macro_queue_item {
    bool is_delay;
    bool use_tap_ms;
    bool pressed;
    uint32_t delay_ms;
    struct zmk_behavior_binding binding;
};

static int on_runtime_macro_wait_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_runtime_macro_wait_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api runtime_macro_wait_driver_api = {
    .binding_pressed = on_runtime_macro_wait_pressed,
    .binding_released = on_runtime_macro_wait_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

DEVICE_DEFINE(runtime_macro_wait_behavior, RUNTIME_MACRO_WAIT_BEHAVIOR_NAME, NULL, NULL, NULL, NULL,
              POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &runtime_macro_wait_driver_api);

static const STRUCT_SECTION_ITERABLE(zmk_behavior_ref, runtime_macro_wait_behavior_ref) = {
    .device = DEVICE_GET(runtime_macro_wait_behavior),
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

static int queue_behavior(const struct zmk_behavior_binding_event *event,
                          const struct zmk_behavior_binding *binding, bool pressed,
                          uint32_t wait_ms) {
    int ret = zmk_behavior_queue_add(event, *binding, pressed, wait_ms);
    if (ret < 0) {
        LOG_WRN("Runtime macro behavior queue add failed: %d", ret);
    }

    return ret;
}

static int queue_runtime_macro(const struct runtime_macro_queue_item *items, size_t count,
                               const struct zmk_behavior_binding_event *event) {
    uint32_t pending_delay_ms = 0;
    struct zmk_behavior_binding none_binding = {
        .behavior_dev = RUNTIME_MACRO_WAIT_BEHAVIOR_NAME,
    };

    for (size_t i = 0; i < count; i++) {
        const struct runtime_macro_queue_item *item = &items[i];

        if (item->is_delay) {
            uint32_t delay_ms = item->use_tap_ms ? get_runtime_macro_tap_ms() : item->delay_ms;
            if (UINT32_MAX - pending_delay_ms < delay_ms) {
                pending_delay_ms = UINT32_MAX;
            } else {
                pending_delay_ms += delay_ms;
            }
            continue;
        }

        if (pending_delay_ms > 0) {
            int ret = queue_behavior(event, &none_binding, true, pending_delay_ms);
            if (ret < 0) {
                return ret;
            }
            pending_delay_ms = 0;
        }

        uint32_t wait_ms = 0;
        if (i + 1 < count && items[i + 1].is_delay) {
            wait_ms = items[i + 1].use_tap_ms ? get_runtime_macro_tap_ms() : items[i + 1].delay_ms;
            i++;
        }

        int ret = queue_behavior(event, &item->binding, item->pressed, wait_ms);
        if (ret < 0) {
            return ret;
        }
    }

    if (pending_delay_ms > 0) {
        return queue_behavior(event, &none_binding, true, pending_delay_ms);
    }

    return 0;
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

    if (name && name_capacity > 0) {
        struct zmk_custom_setting_value name_value;
        int ret = zmk_custom_setting_read_array_by_key(
            ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_NAMES_KEY, index, &name_value);
        if (ret < 0) {
            return ret;
        }

        size_t copy_size = MIN(name_value.size, name_capacity - 1);
        memcpy(name, name_value.string_value, copy_size);
        name[copy_size] = '\0';
    }

    struct zmk_custom_setting_value body_value;
    int ret = zmk_custom_setting_read_array_by_key(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, ZMK_RUNTIME_MACRO_BODIES_KEY, index, &body_value);
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

    return queue_runtime_macro(items, count, event);
}
