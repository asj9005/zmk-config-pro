/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT totem_behavior_thumb_click

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_TOTEM_ESB_COMPAT)
#include <totem/sticky_tap.h>
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct thumb_click_config {
    const struct device *sticky;
    struct zmk_behavior_binding regular;
    struct zmk_behavior_binding click;
};

struct thumb_click_data {
    const struct zmk_behavior_binding *active;
    uint32_t position;
};

static int thumb_click_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct thumb_click_config *config = dev->config;
    struct thumb_click_data *data = dev->data;
    if (data->active != NULL) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    data->active = &config->regular;
    data->position = event.position;
#if IS_ENABLED(CONFIG_TOTEM_ESB_COMPAT)
    /* Release only the tapped thumb before the mouse press enters its queue.
     * Other modifier owners remain active. For an expired tap or a physically
     * held Shift, let the ordinary mod-morph see the current modifiers. */
    if (totem_sticky_consume_tap(config->sticky, binding->param1, event.timestamp)) {
        data->active = &config->click;
    }
#endif
    struct zmk_behavior_binding child = *data->active;
    return zmk_behavior_invoke_binding(&child, event, true);
}

static int thumb_click_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct thumb_click_data *data = dev->data;
    if (data->active == NULL || data->position != event.position) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    struct zmk_behavior_binding child = *data->active;
    data->active = NULL;
    return zmk_behavior_invoke_binding(&child, event, false);
}

static const struct behavior_driver_api thumb_click_driver = {
    .binding_pressed = thumb_click_pressed,
    .binding_released = thumb_click_released,
};

#define THUMB_CLICK_INST(n)                                                                        \
    static struct thumb_click_data thumb_click_data_##n;                                           \
    static const struct thumb_click_config thumb_click_config_##n = {                              \
        .sticky = DEVICE_DT_GET(DT_INST_PHANDLE(n, sticky)),                                       \
        .regular = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                                  \
        .click = ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),                                    \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &thumb_click_data_##n, &thumb_click_config_##n,          \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &thumb_click_driver);

DT_INST_FOREACH_STATUS_OKAY(THUMB_CLICK_INST)

#endif
