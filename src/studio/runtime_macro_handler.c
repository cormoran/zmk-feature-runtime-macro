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

#define RUNTIME_MACRO_RPC_MAX_STEPS 64

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

/* No packed-key-run splitting is needed here: KeyTapSequenceStep.packed_keys
 * stays capped at max_size:64 by the proto (.options unchanged), so every
 * individual MacroStep this function receives already encodes to at most one
 * KEY_TAP_SEQUENCE opcode of <= 64 keys. Raising MacroDetail.steps' max_count
 * to 64 lets a macro carry more *steps* (e.g. more separate sequences/
 * bindings up to CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES total), not longer
 * individual sequences. */
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

struct list_macros_ctx {
    cormoran_runtime_macro_ListMacrosResponse *result;
};

static int list_macros_cb(uint32_t slot, const char *name, size_t encoded_size, void *user_data) {
    struct list_macros_ctx *ctx = user_data;

    if (ctx->result->macros_count >= ARRAY_SIZE(ctx->result->macros)) {
        return -ENOSPC;
    }

    cormoran_runtime_macro_MacroSummary *summary =
        &ctx->result->macros[ctx->result->macros_count++];
    summary->slot = slot;
    snprintf(summary->name, sizeof(summary->name), "%s", name);
    summary->encoded_size = encoded_size;

    return 0;
}

static int handle_list_macros(cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_ListMacrosResponse result =
        cormoran_runtime_macro_ListMacrosResponse_init_zero;

    result.max_macro_bytes = CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES;
    result.max_name_length = CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN;

    struct list_macros_ctx ctx = {.result = &result};
    int ret = zmk_runtime_macro_for_each(list_macros_cb, &ctx);
    if (ret < 0) {
        return ret;
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
    result.settings.max_entries = CONFIG_ZMK_RUNTIME_MACRO_COUNT;
    result.settings.key_press_behavior_id =
        zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp)));
    result.settings.pool_bytes_total = zmk_runtime_macro_pool_total();
    result.settings.pool_bytes_used = zmk_runtime_macro_pool_used();

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

/* Every operational RPC (get/set-step-count/set-step/append-step) addresses
 * a macro by slot number; this resolves the slot to the name the underlying
 * zmk_runtime_macro_read/write() module API still takes and, on failure,
 * sets a slot-specific error message directly on `resp` (rather than letting
 * the generic -ERANGE/-ENOENT dispatch at the bottom of
 * runtime_macro_rpc_handle_request() run, since -ERANGE is ambiguous with
 * unrelated step-index/step-count range checks in the same handlers).
 * Returns -ERANGE if the slot is out of CONFIG_ZMK_RUNTIME_MACRO_COUNT
 * range, -ENOENT if the slot is currently unbound (no live macro there). */
static int resolve_slot_name(uint32_t slot, char *name, size_t name_capacity,
                             cormoran_runtime_macro_Response *resp) {
    int ret = zmk_runtime_macro_name_for_slot(slot, name, name_capacity);
    if (ret == -ERANGE) {
        set_error(resp, "Slot out of range");
    } else if (ret == -ENOENT) {
        set_error(resp, "No macro bound to that slot - create one first with CreateMacro, "
                        "then read its assigned slot from the macro list");
    }
    return ret;
}

static int fill_macro_detail(uint32_t slot, const char *name,
                             cormoran_runtime_macro_MacroDetail *detail) {
    size_t encoded_size = 0;
    uint8_t encoded[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];

    int ret = zmk_runtime_macro_read(name, encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    detail->slot = slot;
    snprintf(detail->name, sizeof(detail->name), "%s", name);
    detail->encoded_size = encoded_size;
    return decode_steps(encoded, encoded_size, detail->steps, &detail->steps_count,
                        ARRAY_SIZE(detail->steps));
}

static int handle_get_macro(const cormoran_runtime_macro_GetMacroRequest *req,
                            cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_GetMacroResponse result =
        cormoran_runtime_macro_GetMacroResponse_init_zero;

    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    int ret = resolve_slot_name(req->slot, name, sizeof(name), resp);
    if (ret < 0) {
        return ret;
    }

    result.has_macro = true;
    ret = fill_macro_detail(req->slot, name, &result.macro);
    if (ret < 0) {
        return ret;
    }

    resp->which_response_type = cormoran_runtime_macro_Response_get_macro_tag;
    resp->response_type.get_macro = result;

    return 0;
}

static int read_macro_steps(const char *name, cormoran_runtime_macro_MacroStep *steps,
                            pb_size_t *steps_count, pb_size_t steps_capacity) {
    uint8_t current_encoded[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t current_encoded_size = 0;

    int ret = zmk_runtime_macro_read(name, current_encoded, sizeof(current_encoded),
                                     &current_encoded_size);
    if (ret < 0) {
        return ret;
    }

    return decode_steps(current_encoded, current_encoded_size, steps, steps_count, steps_capacity);
}

static int write_macro_steps(const char *name, const cormoran_runtime_macro_MacroStep *steps,
                             pb_size_t steps_count, bool persist) {
    uint8_t encoded[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t encoded_size = 0;

    int ret = encode_steps(steps, steps_count, encoded, sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }

    enum zmk_custom_setting_write_mode mode =
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;
    return zmk_runtime_macro_write(name, encoded, encoded_size, mode);
}

static int handle_set_macro_step_count(const cormoran_runtime_macro_SetMacroStepCountRequest *req,
                                       cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_MacroStep steps[RUNTIME_MACRO_RPC_MAX_STEPS];
    pb_size_t steps_count = 0;

    if (req->step_count > ARRAY_SIZE(steps)) {
        return -ERANGE;
    }

    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    int ret = resolve_slot_name(req->slot, name, sizeof(name), resp);
    if (ret < 0) {
        return ret;
    }

    ret = read_macro_steps(name, steps, &steps_count, ARRAY_SIZE(steps));
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

    ret = write_macro_steps(name, steps, steps_count, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro \"%s\" (slot %u) step count updated",
             name, req->slot);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_set_macro_step(const cormoran_runtime_macro_SetMacroStepRequest *req,
                                 cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_MacroStep steps[RUNTIME_MACRO_RPC_MAX_STEPS];
    pb_size_t steps_count = 0;

    if (!req->has_step) {
        return -EINVAL;
    }

    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    int ret = resolve_slot_name(req->slot, name, sizeof(name), resp);
    if (ret < 0) {
        return ret;
    }

    ret = read_macro_steps(name, steps, &steps_count, ARRAY_SIZE(steps));
    if (ret < 0) {
        return ret;
    }

    if (req->step_index >= steps_count) {
        return -ERANGE;
    }

    steps[req->step_index] = req->step;
    ret = write_macro_steps(name, steps, steps_count, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro \"%s\" (slot %u) step %u updated", name,
             req->slot, req->step_index);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_append_macro_step(const cormoran_runtime_macro_AppendMacroStepRequest *req,
                                    cormoran_runtime_macro_Response *resp) {
    cormoran_runtime_macro_MacroStep steps[RUNTIME_MACRO_RPC_MAX_STEPS];
    pb_size_t steps_count = 0;

    if (!req->has_step) {
        return -EINVAL;
    }

    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    int ret = resolve_slot_name(req->slot, name, sizeof(name), resp);
    if (ret < 0) {
        return ret;
    }

    ret = read_macro_steps(name, steps, &steps_count, ARRAY_SIZE(steps));
    if (ret < 0) {
        return ret;
    }

    if (steps_count >= ARRAY_SIZE(steps)) {
        return -ENOSPC;
    }

    steps[steps_count++] = req->step;
    ret = write_macro_steps(name, steps, steps_count, req->persist);
    if (ret < 0) {
        return ret;
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = steps_count;
    snprintf(result.message, sizeof(result.message), "Macro \"%s\" (slot %u) step appended", name,
             req->slot);

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

/* Map the errno a create/rename returns to a clear, actionable message and
 * set it on `resp` directly (returning 0 so the generic errno dispatch at the
 * bottom of runtime_macro_rpc_handle_request() does not overwrite it). `name`
 * is the offending macro name for the interpolated cases. Returns 0 if it
 * handled `ret`, or `ret` unchanged if it is not one of the known cases (let
 * the caller return it for the generic path). */
static int set_lifecycle_error(cormoran_runtime_macro_Response *resp, int ret, const char *name) {
    switch (ret) {
    case -EEXIST:
        set_error(resp, "A macro with that name already exists");
        return 0;
    case -ENOENT:
        set_error(resp, "No macro with that name");
        return 0;
    case -EMSGSIZE:
        set_error(resp, "Macro name is too long");
        return 0;
    case -ENOSPC:
        set_error(resp, "No space for another macro: delete one, or free the pool");
        return 0;
    case -EINVAL:
        set_error(resp, "Invalid macro name");
        return 0;
    default:
        ARG_UNUSED(name);
        return ret;
    }
}

static int handle_create_macro(const cormoran_runtime_macro_CreateMacroRequest *req,
                               cormoran_runtime_macro_Response *resp) {
    if (req->name[0] == '\0') {
        set_error(resp, "Macro name must not be empty");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        req->persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    /* A fresh macro is just the format-version header with no steps; the
     * client adds steps afterwards with AppendMacroStep. */
    const uint8_t empty_body[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION};
    uint32_t slot = 0;
    int ret = zmk_runtime_macro_create(req->name, empty_body, sizeof(empty_body), mode, &slot);
    if (ret < 0) {
        return set_lifecycle_error(resp, ret, req->name);
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro \"%s\" created (slot %u)", req->name,
             slot);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_delete_macro(const cormoran_runtime_macro_DeleteMacroRequest *req,
                               cormoran_runtime_macro_Response *resp) {
    if (req->name[0] == '\0') {
        set_error(resp, "Macro name must not be empty");
        return 0;
    }

    int ret = zmk_runtime_macro_delete(req->name);
    if (ret < 0) {
        return set_lifecycle_error(resp, ret, req->name);
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro \"%s\" deleted", req->name);

    resp->which_response_type = cormoran_runtime_macro_Response_status_tag;
    resp->response_type.status = result;

    return 0;
}

static int handle_rename_macro(const cormoran_runtime_macro_RenameMacroRequest *req,
                               cormoran_runtime_macro_Response *resp) {
    if (req->old_name[0] == '\0' || req->new_name[0] == '\0') {
        set_error(resp, "Macro name must not be empty");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        req->persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    int ret = zmk_runtime_macro_rename(req->old_name, req->new_name, mode);
    if (ret < 0) {
        return set_lifecycle_error(resp, ret, req->new_name);
    }

    cormoran_runtime_macro_StatusResponse result = cormoran_runtime_macro_StatusResponse_init_zero;
    result.affected_count = 1;
    snprintf(result.message, sizeof(result.message), "Macro renamed to \"%s\"", req->new_name);

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

    /* Our mutating handlers write macros through the custom-settings core API,
     * which raises a "setting changed" event that the custom-settings Studio
     * layer turns into its own notification. That notification is redundant
     * here (this RPC's own response already confirms the change to the single
     * Studio client), and its listener runs synchronously in this thread and
     * pb_encodes the notification: at the default CONFIG_ZMK_STUDIO_RPC_THREAD_
     * STACK_SIZE (4096) that extra encode depth OVERFLOWS the RPC thread stack
     * on real hardware (MemManage / watchdog K_ERR_STACK_CHK_FAIL - observed on
     * a XIAO nRF52840 running CreateMacro), and even with headroom it competes
     * with the pending response on the shared transport. Suppress it for the
     * whole dispatch, exactly as custom-settings brackets its own RPC entry
     * point. Read-only requests raise no event, so bracketing them is harmless. */
    zmk_custom_settings_notify_suppress_begin();

    switch (req.which_request_type) {
    case cormoran_runtime_macro_Request_list_macros_tag:
        ret = handle_list_macros(resp);
        break;
    case cormoran_runtime_macro_Request_get_macro_tag:
        ret = handle_get_macro(&req.request_type.get_macro, resp);
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
    case cormoran_runtime_macro_Request_append_macro_step_tag:
        ret = handle_append_macro_step(&req.request_type.append_macro_step, resp);
        break;
    case cormoran_runtime_macro_Request_create_macro_tag:
        ret = handle_create_macro(&req.request_type.create_macro, resp);
        break;
    case cormoran_runtime_macro_Request_delete_macro_tag:
        ret = handle_delete_macro(&req.request_type.delete_macro, resp);
        break;
    case cormoran_runtime_macro_Request_rename_macro_tag:
        ret = handle_rename_macro(&req.request_type.rename_macro, resp);
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

    zmk_custom_settings_notify_suppress_end();

    if (ret < 0 && resp->which_response_type != cormoran_runtime_macro_Response_error_tag) {
        /* resolve_slot_name() already sets a slot-specific error directly on
         * `resp` for its own -ERANGE/-ENOENT (see its doc comment) - the
         * guard above avoids clobbering that with the generic messages
         * below, which cover errors from everything else (e.g. -ENOENT from
         * a raw name-keyed module call, or -ERANGE from an out-of-bounds
         * step_index/step_count that has nothing to do with slot
         * resolution). -ENOSPC from a body write means the shared
         * name+body pool (CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES) is exhausted -
         * give a clear, actionable message instead of the generic errno
         * text; every other failure keeps the generic format. */
        if (ret == -ENOSPC) {
            set_error(resp, "Macro pool full: delete or shrink another macro");
        } else if (ret == -ENOENT) {
            set_error(resp, "No macro bound to that slot - create one first with CreateMacro, "
                            "then read its assigned slot from the macro list");
        } else {
            set_errno_error(resp, "Runtime macro RPC", ret);
        }
    }

    return true;
}
