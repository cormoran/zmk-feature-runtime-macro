/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/runtime_macro/runtime_macro.pb.h>
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RUNTIME_MACRO_RPC_MAX_STEPS 32

static bool runtime_macro_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                             pb_callback_t *encode_response);

static struct zmk_rpc_custom_subsystem_meta runtime_macro_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-runtime-macro/"),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__runtime_macro, &runtime_macro_meta,
                         runtime_macro_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__runtime_macro, cormoran_runtime_macro_Response);

static void set_error(cormoran_runtime_macro_Response *resp, const char *message) {
    cormoran_runtime_macro_ErrorResponse err = cormoran_runtime_macro_ErrorResponse_init_zero;

    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_runtime_macro_Response_error_tag;
    resp->response_type.error = err;
}

static void set_errno_error(cormoran_runtime_macro_Response *resp, const char *operation, int err) {
    cormoran_runtime_macro_ErrorResponse error = cormoran_runtime_macro_ErrorResponse_init_zero;

    snprintf(error.message, sizeof(error.message), "%s failed: %d", operation, err);
    resp->which_response_type = cormoran_runtime_macro_Response_error_tag;
    resp->response_type.error = error;
}

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

static int read_step_binding(const uint8_t *encoded, size_t size, size_t *offset,
                             cormoran_runtime_macro_BehaviorBinding *binding) {
    int ret = read_uvar(encoded, size, offset, &binding->behavior_id);
    if (ret < 0) {
        return ret;
    }
    ret = read_uvar(encoded, size, offset, &binding->param1);
    if (ret < 0) {
        return ret;
    }
    return read_uvar(encoded, size, offset, &binding->param2);
}

static int append_step_binding(uint8_t *dest, size_t capacity, size_t *offset,
                               const cormoran_runtime_macro_BehaviorBinding *binding) {
    int ret = append_uvar(dest, capacity, offset, binding->behavior_id);
    if (ret < 0) {
        return ret;
    }
    ret = append_uvar(dest, capacity, offset, binding->param1);
    if (ret < 0) {
        return ret;
    }
    return append_uvar(dest, capacity, offset, binding->param2);
}

static int read_key_tap_sequence_step(const uint8_t *encoded, size_t size, size_t *offset,
                                      cormoran_runtime_macro_KeyTapSequenceStep *sequence) {
    uint32_t sequence_size;
    int ret = read_uvar(encoded, size, offset, &sequence_size);
    if (ret < 0) {
        return ret;
    }
    if (sequence_size > size - *offset || sequence_size > sizeof(sequence->packed_keys.bytes)) {
        return -EINVAL;
    }

    for (uint32_t i = 0; i < sequence_size; i++) {
        uint32_t keycode;
        ret = zmk_runtime_macro_unpack_key_tap(encoded[*offset + i], &keycode);
        if (ret < 0) {
            return ret;
        }
    }

    sequence->packed_keys.size = sequence_size;
    memcpy(sequence->packed_keys.bytes, &encoded[*offset], sequence_size);
    *offset += sequence_size;
    return 0;
}

static int append_key_tap_sequence_step(uint8_t *dest, size_t capacity, size_t *offset,
                                        const cormoran_runtime_macro_KeyTapSequenceStep *sequence) {
    int ret = append_uvar(dest, capacity, offset, sequence->packed_keys.size);
    if (ret < 0) {
        return ret;
    }

    if (sequence->packed_keys.size > capacity - *offset) {
        return -ENOSPC;
    }

    for (size_t i = 0; i < sequence->packed_keys.size; i++) {
        uint32_t keycode;
        ret = zmk_runtime_macro_unpack_key_tap(sequence->packed_keys.bytes[i], &keycode);
        if (ret < 0) {
            return ret;
        }
        dest[(*offset)++] = sequence->packed_keys.bytes[i];
    }

    return 0;
}

static int decode_steps(const uint8_t *encoded, size_t encoded_size,
                        cormoran_runtime_macro_MacroStep *steps, pb_size_t *steps_count,
                        pb_size_t steps_capacity) {
    *steps_count = 0;

    if (encoded_size == 0) {
        return 0;
    }

    if (encoded[0] != ZMK_RUNTIME_MACRO_FORMAT_VERSION) {
        return -EINVAL;
    }

    size_t offset = 1;
    while (offset < encoded_size) {
        if (*steps_count >= steps_capacity) {
            return -ENOSPC;
        }

        cormoran_runtime_macro_MacroStep *step = &steps[(*steps_count)++];
        *step = (cormoran_runtime_macro_MacroStep)cormoran_runtime_macro_MacroStep_init_zero;

        uint8_t opcode = encoded[offset++];
        int ret;
        switch (opcode) {
        case ZMK_RUNTIME_MACRO_OP_DOWN:
            step->which_step = cormoran_runtime_macro_MacroStep_down_tag;
            break;
        case ZMK_RUNTIME_MACRO_OP_UP:
            step->which_step = cormoran_runtime_macro_MacroStep_up_tag;
            break;
        case ZMK_RUNTIME_MACRO_OP_TAP:
            step->which_step = cormoran_runtime_macro_MacroStep_tap_tag;
            break;
        case ZMK_RUNTIME_MACRO_OP_DELAY:
            step->which_step = cormoran_runtime_macro_MacroStep_delay_tag;
            ret = read_uvar(encoded, encoded_size, &offset, &step->step.delay.delay_ms);
            if (ret < 0) {
                return ret;
            }
            continue;
        case ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE:
            step->which_step = cormoran_runtime_macro_MacroStep_key_tap_sequence_tag;
            ret = read_key_tap_sequence_step(encoded, encoded_size, &offset,
                                             &step->step.key_tap_sequence);
            if (ret < 0) {
                return ret;
            }
            continue;
        default:
            return -EINVAL;
        }

        cormoran_runtime_macro_BehaviorBinding *binding = NULL;
        if (step->which_step == cormoran_runtime_macro_MacroStep_down_tag) {
            binding = &step->step.down;
        } else if (step->which_step == cormoran_runtime_macro_MacroStep_up_tag) {
            binding = &step->step.up;
        } else {
            binding = &step->step.tap;
        }

        ret = read_step_binding(encoded, encoded_size, &offset, binding);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int encode_steps(const cormoran_runtime_macro_MacroStep *steps, pb_size_t steps_count,
                        uint8_t *encoded, size_t encoded_capacity, size_t *encoded_size) {
    if (encoded_capacity == 0) {
        return -ENOSPC;
    }

    encoded[0] = ZMK_RUNTIME_MACRO_FORMAT_VERSION;
    size_t offset = 1;

    for (pb_size_t i = 0; i < steps_count; i++) {
        const cormoran_runtime_macro_MacroStep *step = &steps[i];
        const cormoran_runtime_macro_BehaviorBinding *binding = NULL;
        uint8_t opcode;

        switch (step->which_step) {
        case cormoran_runtime_macro_MacroStep_down_tag:
            opcode = ZMK_RUNTIME_MACRO_OP_DOWN;
            binding = &step->step.down;
            break;
        case cormoran_runtime_macro_MacroStep_up_tag:
            opcode = ZMK_RUNTIME_MACRO_OP_UP;
            binding = &step->step.up;
            break;
        case cormoran_runtime_macro_MacroStep_tap_tag:
            opcode = ZMK_RUNTIME_MACRO_OP_TAP;
            binding = &step->step.tap;
            break;
        case cormoran_runtime_macro_MacroStep_delay_tag:
            opcode = ZMK_RUNTIME_MACRO_OP_DELAY;
            break;
        case cormoran_runtime_macro_MacroStep_key_tap_sequence_tag:
            opcode = ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE;
            break;
        default:
            return -EINVAL;
        }

        if (offset >= encoded_capacity) {
            return -ENOSPC;
        }
        encoded[offset++] = opcode;

        if (opcode == ZMK_RUNTIME_MACRO_OP_DELAY) {
            int ret = append_uvar(encoded, encoded_capacity, &offset, step->step.delay.delay_ms);
            if (ret < 0) {
                return ret;
            }
        } else if (opcode == ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE) {
            int ret = append_key_tap_sequence_step(encoded, encoded_capacity, &offset,
                                                   &step->step.key_tap_sequence);
            if (ret < 0) {
                return ret;
            }
        } else {
            int ret = append_step_binding(encoded, encoded_capacity, &offset, binding);
            if (ret < 0) {
                return ret;
            }
        }
    }

    *encoded_size = offset;
    return 0;
}

static int handle_list_macros(cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_ListMacrosResponse result =
        cormoran_runtime_macro_ListMacrosResponse_init_zero;

    result.max_macro_bytes = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE;
    result.max_name_length = CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE;

    for (uint32_t i = 0; i < CONFIG_ZMK_RUNTIME_MACRO_COUNT; i++) {
        cormoran_runtime_macro_MacroSummary *summary = &result.macros[result.macros_count++];
        uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
        size_t encoded_size = 0;

        summary->index = i;
        int ret = zmk_runtime_macro_read(i, summary->name, sizeof(summary->name), encoded,
                                         sizeof(encoded), &encoded_size);
        if (ret < 0) {
            return ret;
        }

        summary->encoded_size = encoded_size;
    }

    resp->which_response_type = cormoran_runtime_macro_Response_list_macros_tag;
    resp->response_type.list_macros = result;

    return 0;
}

static int read_tap_ms(uint32_t *tap_ms) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_MACRO_TAP_MS_KEY, &value);
    if (ret < 0) {
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value < 0) {
        return -EINVAL;
    }

    *tap_ms = MIN(value.int32_value, 10000);
    return 0;
}

static int handle_get_macro_global_settings(cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_GetMacroGlobalSettingsResponse result =
        cormoran_runtime_macro_GetMacroGlobalSettingsResponse_init_zero;

    result.has_settings = true;
    int ret = read_tap_ms(&result.settings.tap_ms);
    if (ret < 0) {
        return ret;
    }
    result.settings.max_macro = CONFIG_ZMK_RUNTIME_MACRO_COUNT;
    result.settings.key_press_behavior_id =
        zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp)));

    resp->which_response_type = cormoran_runtime_macro_Response_get_macro_global_settings_tag;
    resp->response_type.get_macro_global_settings = result;

    return 0;
}

static int handle_set_tap_ms(const cormoran_runtime_macro_SetTapMsRequest *req,
                             cormoran_runtime_macro_Response *resp) {
    if (req->tap_ms > 10000) {
        return -ERANGE;
    }

    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(req->tap_ms);
    enum zmk_custom_setting_write_mode mode =
        req->persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    int ret = zmk_custom_setting_write_by_key(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                                              ZMK_RUNTIME_MACRO_TAP_MS_KEY, &value, mode);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Runtime macro tap_ms updated");

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int fill_macro_slot(uint32_t index, cormoran_runtime_macro_MacroSlot *slot) {
    size_t encoded_size = 0;
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];

    int ret = zmk_runtime_macro_read(index, slot->name, sizeof(slot->name), encoded,
                                     sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    slot->index = index;
    slot->encoded_size = encoded_size;
    ret = decode_steps(encoded, encoded_size, slot->steps, &slot->steps_count,
                       ARRAY_SIZE(slot->steps));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static int handle_get_macro(const cormoran_runtime_macro_GetMacroRequest *req,
                            cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_GetMacroResponse result =
        cormoran_runtime_macro_GetMacroResponse_init_zero;

    result.has_macro = true;
    int ret = fill_macro_slot(req->index, &result.macro);
    if (ret < 0) {
        return ret;
    }

    resp->which_response_type = cormoran_runtime_macro_Response_get_macro_tag;
    resp->response_type.get_macro = result;

    return 0;
}

static int handle_set_macro_name(const cormoran_runtime_macro_SetMacroNameRequest *req,
                                 cormoran_runtime_macro_Response *resp) {
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    char current_name[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    size_t encoded_size = 0;

    int ret = zmk_runtime_macro_read(req->index, current_name, sizeof(current_name), encoded,
                                     sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_runtime_macro_write(req->index, req->name, encoded, encoded_size, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro %u name updated", req->index);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int read_macro_steps(uint32_t index, char *name, size_t name_size,
                            cormoran_runtime_macro_MacroStep *steps, pb_size_t *steps_count,
                            pb_size_t steps_capacity) {
    uint8_t current_encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t current_encoded_size = 0;

    int ret = zmk_runtime_macro_read(index, name, name_size, current_encoded,
                                     sizeof(current_encoded), &current_encoded_size);
    if (ret < 0) {
        return ret;
    }

    return decode_steps(current_encoded, current_encoded_size, steps, steps_count, steps_capacity);
}

static int write_macro_steps(uint32_t index, const char *name,
                             const cormoran_runtime_macro_MacroStep *steps, pb_size_t steps_count,
                             bool persist) {
    uint8_t encoded[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t encoded_size = 0;

    int ret = encode_steps(steps, steps_count, encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    return zmk_runtime_macro_write(index, name, encoded, encoded_size, persist);
}

static int handle_set_macro_step_count(const cormoran_runtime_macro_SetMacroStepCountRequest *req,
                                       cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_MacroStep steps[RUNTIME_MACRO_RPC_MAX_STEPS];
    pb_size_t steps_count = 0;
    char name[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];

    if (req->step_count > ARRAY_SIZE(steps)) {
        return -ERANGE;
    }

    int ret =
        read_macro_steps(req->index, name, sizeof(name), steps, &steps_count, ARRAY_SIZE(steps));
    if (ret < 0) {
        return ret;
    }

    while (steps_count < req->step_count) {
        cormoran_runtime_macro_MacroStep *step = &steps[steps_count++];
        *step = (cormoran_runtime_macro_MacroStep)cormoran_runtime_macro_MacroStep_init_zero;
        step->which_step = cormoran_runtime_macro_MacroStep_delay_tag;
        step->step.delay.delay_ms = 0;
    }
    steps_count = req->step_count;

    ret = write_macro_steps(req->index, name, steps, steps_count, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro %u step count updated", req->index);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_set_macro_step(const cormoran_runtime_macro_SetMacroStepRequest *req,
                                 cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_MacroStep steps[RUNTIME_MACRO_RPC_MAX_STEPS];
    pb_size_t steps_count = 0;
    char name[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];

    if (!req->has_step) {
        return -EINVAL;
    }

    int ret =
        read_macro_steps(req->index, name, sizeof(name), steps, &steps_count, ARRAY_SIZE(steps));
    if (ret < 0) {
        return ret;
    }

    if (req->step_index >= steps_count) {
        return -ERANGE;
    }

    steps[req->step_index] = req->step;
    ret = write_macro_steps(req->index, name, steps, steps_count, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro %u step %u updated", req->index,
             req->step_index);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_delete_macro(const cormoran_runtime_macro_DeleteMacroRequest *req,
                               cormoran_runtime_macro_Response *resp) {
    int ret = zmk_runtime_macro_write(req->index, "", NULL, 0, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro %u deleted", req->index);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_save_macros(cormoran_runtime_macro_Response *resp) {
    uint32_t affected_count = 0;
    int ret =
        zmk_custom_settings_save_scope(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, NULL, &affected_count);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = affected_count;
    snprintf(result.message, sizeof(result.message), "Runtime macro settings saved");

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_discard_macros(cormoran_runtime_macro_Response *resp) {
    uint32_t affected_count = 0;
    int ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, NULL,
                                                &affected_count);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = affected_count;
    snprintf(result.message, sizeof(result.message), "Runtime macro settings discarded");

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static bool runtime_macro_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                             pb_callback_t *encode_response) {
    cormoran_runtime_macro_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__runtime_macro, encode_response);

    cormoran_runtime_macro_Request req = cormoran_runtime_macro_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);

    if (!pb_decode(&req_stream, cormoran_runtime_macro_Request_fields, &req)) {
        LOG_WRN("Failed to decode runtime macro request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    int ret = 0;

    switch (req.which_request_type) {
    case cormoran_runtime_macro_Request_list_macros_tag:
        ret = handle_list_macros(resp);
        break;
    case cormoran_runtime_macro_Request_get_macro_tag:
        ret = handle_get_macro(&req.request_type.get_macro, resp);
        break;
    case cormoran_runtime_macro_Request_set_macro_name_tag:
        ret = handle_set_macro_name(&req.request_type.set_macro_name, resp);
        break;
    case cormoran_runtime_macro_Request_set_macro_step_count_tag:
        ret = handle_set_macro_step_count(&req.request_type.set_macro_step_count, resp);
        break;
    case cormoran_runtime_macro_Request_get_macro_global_settings_tag:
        ret = handle_get_macro_global_settings(resp);
        break;
    case cormoran_runtime_macro_Request_set_tap_ms_tag:
        ret = handle_set_tap_ms(&req.request_type.set_tap_ms, resp);
        break;
    case cormoran_runtime_macro_Request_set_macro_step_tag:
        ret = handle_set_macro_step(&req.request_type.set_macro_step, resp);
        break;
    case cormoran_runtime_macro_Request_delete_macro_tag:
        ret = handle_delete_macro(&req.request_type.delete_macro, resp);
        break;
    case cormoran_runtime_macro_Request_save_macros_tag:
        ret = handle_save_macros(resp);
        break;
    case cormoran_runtime_macro_Request_discard_macros_tag:
        ret = handle_discard_macros(resp);
        break;
    default:
        ret = -ENOTSUP;
        break;
    }

    if (ret < 0) {
        set_errno_error(resp, "Runtime macro RPC", ret);
    }

    return true;
}
