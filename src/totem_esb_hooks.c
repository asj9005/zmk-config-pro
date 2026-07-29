/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <totem/esb_benchmark.h>

#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&         \
    IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/split/transport/types.h>

struct pending_usb_event {
    uint8_t source;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t rx_tick;
    uint8_t position;
    uint8_t pressed;
};

K_MSGQ_DEFINE(pending_usb_events, sizeof(struct pending_usb_event), 64, 4);
#endif

#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/split/central.h>
#endif

LOG_MODULE_REGISTER(totem_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static atomic_t link_metrics[TOTEM_ESB_LINK_METRIC_COUNT];

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
struct peer_state {
    bool seen;
    int64_t last_seen;
};

static struct peer_state peers[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT];
static void peer_timeout_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(peer_timeout_work, peer_timeout_work_handler);
#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
static void display_sync_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(display_sync_work, display_sync_work_handler);
static uint8_t display_sync_source;
static uint8_t display_sync_remaining;
#endif

static void publish_peer_display(uint8_t source, bool connected) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
    raise_zmk_split_central_status_changed((struct zmk_split_central_status_changed){
        .slot = source,
        .connected = connected,
    });
#else
    ARG_UNUSED(source);
    ARG_UNUSED(connected);
#endif
}

static void publish_peer(uint8_t source, bool connected) {
    publish_peer_display(source, connected);
    totem_esb_notify_transport_status();
    totem_esb_schedule_display_sync();
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
static void display_sync_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (display_sync_remaining == 0) {
        return;
    }

    uint8_t source = display_sync_source++;
    display_sync_remaining--;
    publish_peer_display(source, peers[source].seen);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t level;
    if (peers[source].seen &&
        zmk_split_central_get_peripheral_battery_level(source, &level) == 0) {
        raise_zmk_peripheral_battery_state_changed(
            (struct zmk_peripheral_battery_state_changed){
                .source = source,
                .state_of_charge = level,
            });
    }
#endif

    if (display_sync_remaining > 0) {
        /*
         * Publish one source per tick because the pinned Prospector listener
         * has one state/work item and can coalesce back-to-back source events.
         * A bounded full pass also completes when the last peer disconnects,
         * without continuously waking the display while the link is stable.
         */
        k_work_reschedule(&display_sync_work, K_MSEC(500));
    }
}
#endif

void totem_esb_schedule_display_sync(void) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
    display_sync_source = 0;
    display_sync_remaining = ARRAY_SIZE(peers);
    /*
     * Let the event manager commit the triggering transition/battery update
     * before republishing authoritative state.
     */
    k_work_reschedule(&display_sync_work, K_MSEC(100));
#endif
}

void totem_esb_peer_seen(uint8_t source) {
    if (source >= ARRAY_SIZE(peers)) {
        return;
    }

    bool newly_connected = !peers[source].seen;
    if (newly_connected) {
        peers[source].seen = true;
        publish_peer(source, true);
    }
    peers[source].last_seen = k_uptime_get();
    if (newly_connected) {
        k_work_reschedule(&peer_timeout_work,
                          K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
    }
}

uint8_t totem_esb_peer_connected_count(void) {
    uint8_t count = 0;
    for (uint8_t source = 0; source < ARRAY_SIZE(peers); source++) {
        count += peers[source].seen ? 1 : 0;
    }
    return count;
}

bool totem_esb_peer_is_connected(uint8_t source) {
    return source < ARRAY_SIZE(peers) && peers[source].seen;
}

static void peer_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    bool any_seen = false;

    for (uint8_t source = 0; source < ARRAY_SIZE(peers); source++) {
        if (peers[source].seen &&
            now - peers[source].last_seen > CONFIG_TOTEM_ESB_PEER_TIMEOUT_MS) {
            peers[source].seen = false;
            totem_esb_source_disconnected(source);
            publish_peer(source, false);
        }
        any_seen |= peers[source].seen;
    }

    if (any_seen) {
        k_work_reschedule(&peer_timeout_work,
                          K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
    }
}
#else
void totem_esb_peer_seen(uint8_t source) { ARG_UNUSED(source); }
bool totem_esb_peer_is_connected(uint8_t source) {
    ARG_UNUSED(source);
    return false;
}
uint8_t totem_esb_peer_connected_count(void) { return 0; }
void totem_esb_schedule_display_sync(void) {}
#endif

void totem_esb_benchmark_rx(uint8_t source, uint32_t session_id, uint32_t sequence,
                            uint32_t gap, uint32_t source_tick, uint8_t wire_type,
                            uint8_t event_type, uint8_t position, uint8_t pressed,
                            bool accepted_for_zmk) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    uint32_t dongle_tick = k_cycle_get_32();
    LOG_INF("BENCH_RX source=%u session=%u seq=%u gap=%u source_tick=%u dongle_tick=%u "
            "wire=%u event=%u position=%u pressed=%u accepted=%u",
            source, session_id, sequence, gap, source_tick, dongle_tick, wire_type,
            event_type, position, pressed, accepted_for_zmk);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_USB)
    if (accepted_for_zmk && wire_type == 0 &&
        event_type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        struct pending_usb_event pending = {
            .source = source,
            .session_id = session_id,
            .sequence = sequence,
            .rx_tick = dongle_tick,
            .position = position,
            .pressed = pressed,
        };
        if (k_msgq_put(&pending_usb_events, &pending, K_NO_WAIT) != 0) {
            LOG_WRN("BENCH_USB_QUEUE_OVERFLOW source=%u seq=%u", source, sequence);
        }
    }
#endif
#else
    ARG_UNUSED(source);
    ARG_UNUSED(session_id);
    ARG_UNUSED(sequence);
    ARG_UNUSED(gap);
    ARG_UNUSED(source_tick);
    ARG_UNUSED(wire_type);
    ARG_UNUSED(event_type);
    ARG_UNUSED(position);
    ARG_UNUSED(pressed);
    ARG_UNUSED(accepted_for_zmk);
#endif
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&         \
    IS_ENABLED(CONFIG_ZMK_USB)
int __real_zmk_usb_hid_send_keyboard_report(void);

int __wrap_zmk_usb_hid_send_keyboard_report(void) {
    uint32_t queue_enter_tick = k_cycle_get_32();
    int result = __real_zmk_usb_hid_send_keyboard_report();
    uint32_t queue_done_tick = k_cycle_get_32();
    struct pending_usb_event pending;

    if (k_msgq_get(&pending_usb_events, &pending, K_NO_WAIT) == 0) {
        LOG_INF("BENCH_USB source=%u session=%u seq=%u position=%u pressed=%u "
                "rx_tick=%u queue_enter_tick=%u queue_done_tick=%u result=%d",
                pending.source, pending.session_id, pending.sequence, pending.position,
                pending.pressed, pending.rx_tick, queue_enter_tick, queue_done_tick, result);
    } else {
        LOG_INF("BENCH_USB_UNMATCHED queue_enter_tick=%u queue_done_tick=%u result=%d",
                queue_enter_tick, queue_done_tick, result);
    }

    return result;
}
#endif

void totem_esb_benchmark_rx_invalid(uint8_t pipe, int error) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_RX_INVALID pipe=%u error=%d dongle_tick=%u", pipe, error, k_cycle_get_32());
#else
    ARG_UNUSED(pipe);
    ARG_UNUSED(error);
#endif
}

void totem_esb_benchmark_rx_overflow(uint8_t pipe, uint32_t count) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_RX_OVERFLOW pipe=%u count=%u dongle_tick=%u", pipe, count,
            k_cycle_get_32());
#else
    ARG_UNUSED(pipe);
    ARG_UNUSED(count);
#endif
}

void totem_esb_benchmark_link_metric(uint8_t source, uint32_t session_id, uint8_t metric,
                                     uint32_t value) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_LINK source=%u session=%u metric=%u value=%u dongle_tick=%u", source,
            session_id, metric, value, k_cycle_get_32());
#else
    ARG_UNUSED(source);
    ARG_UNUSED(session_id);
    ARG_UNUSED(metric);
    ARG_UNUSED(value);
#endif
}

void totem_esb_benchmark_tx(uint8_t source, uint16_t message_id, uint16_t attempts,
                            bool success) {
    if (!IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
        atomic_inc(&link_metrics[TOTEM_ESB_LINK_METRIC_TX_MESSAGES]);
        atomic_add(&link_metrics[TOTEM_ESB_LINK_METRIC_TX_ATTEMPTS], attempts);
        if (!success) {
            atomic_inc(&link_metrics[TOTEM_ESB_LINK_METRIC_TX_FAILURES]);
        }
    }
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_TX source=%u msg=%u attempts=%u retransmissions=%u success=%u "
            "source_tick=%u",
            source, message_id, attempts, attempts > 0 ? attempts - 1U : 0U, success,
            k_cycle_get_32());
#else
    ARG_UNUSED(source);
    ARG_UNUSED(message_id);
    ARG_UNUSED(attempts);
    ARG_UNUSED(success);
#endif
}

void totem_esb_transport_queue_pressure(bool producer_ring) {
    if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
        return;
    }
    atomic_inc(&link_metrics[producer_ring
                                 ? TOTEM_ESB_LINK_METRIC_PRODUCER_QUEUE_OVERFLOW
                                 : TOTEM_ESB_LINK_METRIC_APP_QUEUE_PRESSURE]);
}

uint32_t totem_esb_link_metric_value(uint8_t metric) {
    if (metric >= ARRAY_SIZE(link_metrics)) {
        return 0;
    }
    return (uint32_t)atomic_get(&link_metrics[metric]);
}
