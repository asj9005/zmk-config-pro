/*
 * Copyright (c) 2026 The Totem Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT totem_behavior_owned_swapper

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#include <totem/owned_swapper.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* TESTABLE_BEGIN */
#define SWAPPER_RETRY_MIN_MS 25
#define SWAPPER_RETRY_MAX_MS 1000

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define SWAPPER_REMOTE_SOURCE_COUNT ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT
#else
#define SWAPPER_REMOTE_SOURCE_COUNT 0
#endif

#if SWAPPER_REMOTE_SOURCE_COUNT > 0
/* A cleanup can finish while another hold-tap still retains old position
 * events. Their original timestamps must not create a fresh Alt latch later.
 * LOCAL has no array slot: this boundary belongs to remote ESB sources. */
static int64_t source_reset_cutoff[SWAPPER_REMOTE_SOURCE_COUNT];
static bool source_reset_valid[SWAPPER_REMOTE_SOURCE_COUNT];
#endif

struct owned_swapper_config {
    const uint32_t *ignored_positions;
    size_t ignored_count;
};

struct owned_swapper_state {
    const struct owned_swapper_config *config;
    uint32_t position;
    uint8_t source;
    bool active;
    bool pressed;
    bool alt_owned;
    bool tab_pressed;
    bool report_pending;
    int64_t retry_at;
    uint32_t retry_ms;
};

static struct owned_swapper_state swapper = {.retry_ms = SWAPPER_RETRY_MIN_MS};
static K_MUTEX_DEFINE(swapper_mutex);
static void swapper_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(swapper_work, swapper_work_handler);

static uint8_t event_source(struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    return event.source;
#else
    (void)event;
    return ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif
}

static bool swapper_source_activation_blocked(uint8_t source, int64_t timestamp) {
    if (source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return false;
    }
#if SWAPPER_REMOTE_SOURCE_COUNT > 0
    if (source < SWAPPER_REMOTE_SOURCE_COUNT) {
        return source_reset_valid[source] && timestamp <= source_reset_cutoff[source];
    }
#else
    (void)timestamp;
#endif
    /* Reject an invalid remote source before indexing or modifying a latch. */
    return true;
}

static bool swapper_usb_suspended(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB &&
           zmk_usb_get_status() == USB_DC_SUSPEND;
#else
    return false;
#endif
}

/* Retry the current HID report, never replay a semantic Alt press/release. */
static int swapper_send_report(void) {
    bool was_suspended = swapper_usb_suspended();
    int err = zmk_endpoint_send_report(HID_USAGE_KEY);
    /* Pinned USB returns wakeup_request()'s success while suspended without
     * sending a HID packet. Keep a report pending across that successful wake.
     * Checking both sides also handles a resume/suspend concurrent with send. */
    if (err == 0 && (was_suspended || swapper_usb_suspended())) {
        err = -EAGAIN;
    }
    if (err < 0) {
        swapper.report_pending = true;
        swapper.retry_at = k_uptime_get() + swapper.retry_ms;
        swapper.retry_ms = MIN(swapper.retry_ms * 2U, SWAPPER_RETRY_MAX_MS);
    } else {
        swapper.report_pending = false;
        swapper.retry_ms = SWAPPER_RETRY_MIN_MS;
    }
    return err;
}

static void swapper_schedule(void) {
    /* This work item retries reports only. It never ends an idle swapper. */
    if (swapper.report_pending) {
        int64_t remaining = MAX(INT64_C(0), swapper.retry_at - k_uptime_get());
        (void)k_work_reschedule(&swapper_work, K_MSEC(remaining));
    } else {
        (void)k_work_cancel_delayable(&swapper_work);
    }
}

static void swapper_release_tab(int64_t timestamp) {
    if (!swapper.tab_pressed) {
        return;
    }
    /* Clear ownership before callbacks, so a reentrant close cannot release twice. */
    swapper.tab_pressed = false;
    int err = raise_zmk_keycode_state_changed_from_encoded(TAB, false, timestamp);
    if (err < 0) {
        /* A failing listener may have interrupted propagation before the HID
         * listener. Ordinary key release is idempotent (unlike modifier counts).
         * This fallback only clears our Tab; do not use it for Alt. */
        (void)zmk_hid_keyboard_release(ZMK_HID_USAGE_ID(TAB));
        LOG_WRN("Swapper Tab release listener failed: %d", err);
    }
}

static void swapper_notify_alt_change(zmk_mod_flags_t before) {
    zmk_mod_flags_t after = zmk_hid_get_explicit_mods();
    if (((before ^ after) & MOD_LALT) != 0) {
        /* This reports an already-completed aggregate state change. It is not
         * a keycode event and must never add/remove a modifier reference. */
        int err = raise_zmk_modifiers_state_changed((struct zmk_modifiers_state_changed){
            .modifiers = MOD_LALT,
            .state = (after & MOD_LALT) != 0,
        });
        if (err < 0) {
            LOG_WRN("Swapper modifier notification failed: %d", err);
        }
    }
}

static void swapper_close(int64_t timestamp) {
    swapper.active = false;
    swapper.pressed = false;
    swapper_release_tab(timestamp);
    if (swapper.alt_owned) {
        /* Exactly one reference was registered on start. Never flip global
         * state, clear all modifiers, or unregister again on send failure. */
        swapper.alt_owned = false;
        zmk_mod_flags_t before = zmk_hid_get_explicit_mods();
        int err = zmk_hid_unregister_mods(MOD_LALT);
        if (err < 0) {
            LOG_ERR("Swapper Alt reference was already absent: %d", err);
        }
        swapper_notify_alt_change(before);
    }
    (void)swapper_send_report();
}

static int owned_swapper_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct owned_swapper_config *config = dev->config;
    uint8_t source = event_source(event);
    k_mutex_lock(&swapper_mutex, K_FOREVER);

    /* Check before closing/replacing any current latch. Millisecond equality
     * is deliberately rejected: it cannot be distinguished from old input. */
    if (swapper_source_activation_blocked(source, event.timestamp)) {
        k_mutex_unlock(&swapper_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (swapper.active &&
        (swapper.position != event.position || swapper.source != source)) {
        swapper_close(event.timestamp);
    }
    if (swapper.pressed) {
        /* Duplicate physical/behavior presses must not register another Alt. */
        k_mutex_unlock(&swapper_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (!swapper.active) {
        zmk_mod_flags_t before = zmk_hid_get_explicit_mods();
        int err = zmk_hid_register_mods(MOD_LALT);
        if (err < 0) {
            k_mutex_unlock(&swapper_mutex);
            return err;
        }
        swapper.alt_owned = true;
        swapper.active = true;
        swapper.config = config;
        swapper.position = event.position;
        swapper.source = source;
        swapper_notify_alt_change(before);
    }

    swapper.pressed = true;
    swapper.tab_pressed = true;
    /* Tab still uses ZMK's normal keycode event path, including sticky-key
     * consumption. Directly owned Alt contributes its own modifier reference. */
    int err = raise_zmk_keycode_state_changed_from_encoded(TAB, true, event.timestamp);
    if (err < 0) {
        /* The listener may have changed HID state before failing. */
        swapper_close(event.timestamp);
    } else {
        (void)swapper_send_report();
    }
    swapper_schedule();
    k_mutex_unlock(&swapper_mutex);
    return err < 0 ? err : ZMK_BEHAVIOR_OPAQUE;
}

static int owned_swapper_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    (void)binding;
    k_mutex_lock(&swapper_mutex, K_FOREVER);
    /* A delayed release from the old session must not release a newer Tab. */
    if (!swapper_source_activation_blocked(event_source(event), event.timestamp) &&
        swapper.active && swapper.position == event.position &&
        swapper.source == event_source(event)) {
        swapper.pressed = false;
        swapper_release_tab(event.timestamp);
        (void)swapper_send_report();
        swapper_schedule();
    }
    k_mutex_unlock(&swapper_mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int swapper_ignored_index(uint32_t position) {
    for (size_t i = 0; i < swapper.config->ignored_count; i++) {
        if (swapper.config->ignored_positions[i] == position) {
            return (int)i;
        }
    }
    return -1;
}

static int owned_swapper_position(const struct zmk_position_state_changed *event) {
    k_mutex_lock(&swapper_mutex, K_FOREVER);
    if (swapper.active &&
        !(swapper.position == event->position && swapper.source == event->source) &&
        swapper_ignored_index(event->position) < 0) {
        /* Both raw and replay listeners use this same idempotent handler.
         * Preserve press/release interruption, including combo-consumed raw
         * keys and events replayed after a hold-tap activates this behavior. */
        swapper_close(event->timestamp);
        swapper_schedule();
    }
    k_mutex_unlock(&swapper_mutex);
    return ZMK_EV_EVENT_BUBBLE;
}

static int owned_swapper_layer(const struct zmk_layer_state_changed *event) {
    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_mutex_lock(&swapper_mutex, K_FOREVER);
    if (swapper.active) {
        /* The current swapper has no ignored-layers. Preserve activation-only
         * interruption; an ordinary Nav-thumb release is handled above. */
        swapper_close(k_uptime_get());
        swapper_schedule();
    }
    k_mutex_unlock(&swapper_mutex);
    return ZMK_EV_EVENT_BUBBLE;
}

int totem_owned_swapper_replayed_position(const struct zmk_position_state_changed *event) {
    return owned_swapper_position(event);
}

void totem_owned_swapper_source_reset(uint8_t source) {
#if SWAPPER_REMOTE_SOURCE_COUNT > 0
    if (source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL || source >= SWAPPER_REMOTE_SOURCE_COUNT) {
        return;
    }
    k_mutex_lock(&swapper_mutex, K_FOREVER);
    if (swapper.active && swapper.source == source) {
        swapper_close(k_uptime_get());
        swapper_schedule();
    }
    /* Record after synchronous release callbacks and their report attempts.
     * Also fence an inactive source whose old events may still be captured. */
    source_reset_cutoff[source] = k_uptime_get();
    source_reset_valid[source] = true;
    k_mutex_unlock(&swapper_mutex);
#else
    (void)source;
#endif
}

static void swapper_work_handler(struct k_work *work) {
    (void)work;
    k_mutex_lock(&swapper_mutex, K_FOREVER);
    /* A stale/submitted callback must never release Alt or replay key events.
     * It may only retry the current HID report when that report is pending. */
    if (swapper.report_pending && k_uptime_get() >= swapper.retry_at) {
        (void)swapper_send_report();
    }
    swapper_schedule();
    k_mutex_unlock(&swapper_mutex);
}
/* TESTABLE_END */

static int owned_swapper_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL) {
        return owned_swapper_position(position);
    }
    const struct zmk_layer_state_changed *layer = as_zmk_layer_state_changed(event);
    return layer != NULL ? owned_swapper_layer(layer) : ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_owned_swapper, owned_swapper_listener);
ZMK_SUBSCRIPTION(totem_owned_swapper, zmk_position_state_changed);
ZMK_SUBSCRIPTION(totem_owned_swapper, zmk_layer_state_changed);

static const struct behavior_driver_api owned_swapper_driver_api = {
    .binding_pressed = owned_swapper_pressed,
    .binding_released = owned_swapper_released,
};

#define OWNED_SWAPPER_INST(n)                                                                       \
    static const uint32_t ignored_positions_##n[] = DT_INST_PROP(n, ignored_key_positions);          \
    static const struct owned_swapper_config owned_swapper_config_##n = {                           \
        .ignored_positions = ignored_positions_##n,                                                \
        .ignored_count = DT_INST_PROP_LEN(n, ignored_key_positions),                                \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &owned_swapper_config_##n, POST_KERNEL,              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &owned_swapper_driver_api);

DT_INST_FOREACH_STATUS_OKAY(OWNED_SWAPPER_INST)

#else

void totem_owned_swapper_source_reset(uint8_t source) { (void)source; }

int totem_owned_swapper_replayed_position(const struct zmk_position_state_changed *event) {
    (void)event;
    return ZMK_EV_EVENT_BUBBLE;
}

#endif
