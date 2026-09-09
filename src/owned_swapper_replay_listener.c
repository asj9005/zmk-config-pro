/* SPDX-License-Identifier: MIT */

#include <zephyr/devicetree.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <totem/owned_swapper.h>

#if DT_HAS_COMPAT_STATUS_OKAY(totem_behavior_owned_swapper)

/* This listener is deliberately after hold-tap/combo/tap-dance and before
 * keymap. Replayed positions must end an already active swapper before the
 * replayed key is handled. Like the early listener, it only ends an active
 * swapper for nonignored positions; it does not impose an idle timeout. */
static int owned_swapper_replay_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL) {
        totem_owned_swapper_replayed_position(position);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_owned_swapper_replay, owned_swapper_replay_listener);
ZMK_SUBSCRIPTION(totem_owned_swapper_replay, zmk_position_state_changed);

#endif
