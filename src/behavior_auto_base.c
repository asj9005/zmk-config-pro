/*
 * Copyright (c) 2026 The Totem Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT totem_behavior_auto_base

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * A zero-wait macro still waits behind another macro's delayed work. A hold-tap
 * releases its captured position events as soon as the tap behavior returns,
 * so the key tap and return to Base must both finish in this call. Use the same
 * encoded-key event helper as &kp to preserve modifiers and HID usage pages.
 */
static int auto_base_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    int press_err =
        raise_zmk_keycode_state_changed_from_encoded(binding->param1, true, event.timestamp);
    /* A listener can fail after changing state: always attempt the release. */
    int release_err =
        raise_zmk_keycode_state_changed_from_encoded(binding->param1, false, event.timestamp);
    /* Match &to 0: Mouse may have been locked active by the thumb's &to. */
    int layer_err = zmk_keymap_layer_to(0, true);

    if (press_err < 0) {
        LOG_ERR("Auto Base key press failed: %d", press_err);
    }
    if (release_err < 0) {
        LOG_ERR("Auto Base key release failed: %d", release_err);
    }
    if (layer_err < 0) {
        LOG_ERR("Auto Base layer change failed: %d", layer_err);
    }

    if (press_err < 0) {
        return press_err;
    }
    if (release_err < 0) {
        return release_err;
    }
    return layer_err < 0 ? layer_err : ZMK_BEHAVIOR_OPAQUE;
}

static int auto_base_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    /* The full tap was emitted on press, including when invoked by a hold-tap. */
    (void)binding;
    (void)event;
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Key",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE,
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static const struct behavior_driver_api auto_base_driver_api = {
    .binding_pressed = auto_base_pressed,
    .binding_released = auto_base_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define AUTO_BASE_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &auto_base_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AUTO_BASE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
