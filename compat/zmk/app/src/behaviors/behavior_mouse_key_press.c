/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_key_press

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <totem/esb_diagnostics.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "MB1", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB1},
    {.display_name = "MB2", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB2},
    {.display_name = "MB3", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB3},
    {.display_name = "MB4", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB4},
    {.display_name = "MB5", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = MB5}};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

/* TESTABLE_BEGIN */
#define MOUSE_BUTTON_QUEUE_SIZE 64
#define MOUSE_BUTTON_BATCH_SIZE 8
#define MOUSE_BUTTON_RETRY_MS 4
#define MOUSE_BUTTON_MASK ((1U << ZMK_HID_MOUSE_NUM_BUTTONS) - 1U)
BUILD_ASSERT(ZMK_HID_MOUSE_NUM_BUTTONS > 0 && ZMK_HID_MOUSE_NUM_BUTTONS <= 8);

struct mouse_button_packet {
    uint8_t mask;
    uint8_t pressed;
};

struct mouse_button_data {
    const struct device *dev;
    struct k_work_delayable work;
    struct k_spinlock lock;
    struct mouse_button_packet packets[MOUSE_BUTTON_QUEUE_SIZE];
    uint8_t head;
    uint8_t count;
    /* ZMK HID button ownership is reference counted: overlapping bindings
     * must retain separate releases, including during overflow recovery. */
    uint32_t desired[ZMK_HID_MOUSE_NUM_BUTTONS];
    uint32_t accepted[ZMK_HID_MOUSE_NUM_BUTTONS];
    bool resync;
};

static void mouse_button_work(struct k_work *work) {
    struct mouse_button_data *data =
        CONTAINER_OF(k_work_delayable_from_work(work), struct mouse_button_data, work);
    for (unsigned int sent = 0; sent < MOUSE_BUTTON_BATCH_SIZE; sent++) {
        k_spinlock_key_t key = k_spin_lock(&data->lock);
        if (data->count == 0 && data->resync) {
            uint8_t changed = 0;
            uint8_t pressed = 0;
            for (unsigned int i = 0; i < ZMK_HID_MOUSE_NUM_BUTTONS; i++) {
                if (data->desired[i] != data->accepted[i]) {
                    changed |= BIT(i);
                    if (data->desired[i] > data->accepted[i]) {
                        pressed |= BIT(i);
                    }
                }
            }
            data->resync = changed != 0;
            if (changed != 0) {
                data->packets[data->head] = (struct mouse_button_packet){changed, pressed};
                data->count = 1;
            }
        }
        if (data->count == 0) {
            k_spin_unlock(&data->lock, key);
            return;
        }
        struct mouse_button_packet packet = data->packets[data->head];
        unsigned int button = 0;
        while ((packet.mask & BIT(button)) == 0) {
            button++;
        }
        uint8_t bit = BIT(button);
        k_spin_unlock(&data->lock, key);

        /* Input's K_FOREVER is silently reduced to K_NO_WAIT on the system
         * queue. Retain each transition instead, including the last release.
         * A combined mask keeps its final sync pending across partial writes. */
        int err = input_report_key(data->dev, INPUT_BTN_0 + button,
                                   (packet.pressed & bit) != 0, packet.mask == bit, K_NO_WAIT);
        if (err != 0) {
            totem_esb_diag_event(TOTEM_DIAG_INPUT_RETRY, err);
            k_work_reschedule(&data->work, K_MSEC(MOUSE_BUTTON_RETRY_MS));
            return;
        }
        key = k_spin_lock(&data->lock);
        /* Producers only append/update desired; they never replace this head. */
        if ((packet.pressed & bit) != 0) {
            data->accepted[button]++;
        } else {
            data->accepted[button]--;
        }
        data->packets[data->head].mask &= (uint8_t)~bit;
        if (data->packets[data->head].mask == 0) {
            data->head = (data->head + 1U) % MOUSE_BUTTON_QUEUE_SIZE;
            data->count--;
        }
        k_spin_unlock(&data->lock, key);
    }
    /* Bound each invocation so typing, USB and radio work can also run. */
    k_work_reschedule(&data->work, K_NO_WAIT);
}

static void process_key_state(const struct device *dev, int32_t val, bool pressed) {
    struct mouse_button_data *data = dev->data;
    uint8_t mask = (uint32_t)val & MOUSE_BUTTON_MASK;
    if (mask == 0) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    for (unsigned int i = 0; i < ZMK_HID_MOUSE_NUM_BUTTONS; i++) {
        if ((mask & BIT(i)) == 0) {
            continue;
        }
        if (pressed) {
            data->desired[i]++;
        } else if (data->desired[i] != 0) {
            data->desired[i]--;
        } else {
            /* Match HID's rejection of an unmatched release without queuing
             * an edge that could consume another device's button reference. */
            mask &= (uint8_t)~BIT(i);
        }
    }
    if (mask == 0) {
        k_spin_unlock(&data->lock, key);
        return;
    }
    bool overflow = data->resync || data->count == MOUSE_BUTTON_QUEUE_SIZE;
    if (overflow) {
        /* Finite storage may coalesce taps, but never forget the final state. */
        data->resync = true;
    } else {
        uint8_t tail = (data->head + data->count) % MOUSE_BUTTON_QUEUE_SIZE;
        data->packets[tail] = (struct mouse_button_packet){mask, pressed ? mask : 0};
        data->count++;
    }
    k_spin_unlock(&data->lock, key);
    if (overflow) {
        totem_esb_diag_event(TOTEM_DIAG_INPUT_OVERFLOW, 0);
    }
    k_work_reschedule(&data->work, K_NO_WAIT);
}

static int mouse_button_init(const struct device *dev) {
    struct mouse_button_data *data = dev->data;
    data->dev = dev;
    k_work_init_delayable(&data->work, mouse_button_work);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    process_key_state(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, true);

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    process_key_state(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, false);

    return 0;
}
/* TESTABLE_END */

static const struct behavior_driver_api behavior_mouse_key_press_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define MKP_INST(n)                                                                                \
    static struct mouse_button_data mouse_button_data_##n;                                       \
    BEHAVIOR_DT_INST_DEFINE(n, mouse_button_init, NULL, &mouse_button_data_##n, NULL, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_mouse_key_press_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MKP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
