/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <totem/esb_benchmark.h>

#if IS_ENABLED(CONFIG_TOTEM_ESB_PROSPECTOR)
#include <zmk/events/split_central_status_changed.h>
#endif

LOG_MODULE_REGISTER(totem_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
struct peer_state {
    bool seen;
    int64_t last_seen;
};

static struct peer_state peers[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT];
static void peer_timeout_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(peer_timeout_work, peer_timeout_work_handler);

static void publish_peer(uint8_t source, bool connected) {
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

void totem_esb_peer_seen(uint8_t source) {
    if (source >= ARRAY_SIZE(peers)) {
        return;
    }

    if (!peers[source].seen) {
        peers[source].seen = true;
        publish_peer(source, true);
    }
    peers[source].last_seen = k_uptime_get();
    k_work_reschedule(&peer_timeout_work, K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
}

uint8_t totem_esb_peer_connected_count(void) {
    uint8_t count = 0;
    for (uint8_t source = 0; source < ARRAY_SIZE(peers); source++) {
        count += peers[source].seen ? 1 : 0;
    }
    return count;
}

static void peer_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    bool any_seen = false;

    for (uint8_t source = 0; source < ARRAY_SIZE(peers); source++) {
        if (peers[source].seen &&
            now - peers[source].last_seen > CONFIG_TOTEM_ESB_PEER_TIMEOUT_MS) {
            peers[source].seen = false;
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
uint8_t totem_esb_peer_connected_count(void) { return 0; }
#endif

void totem_esb_benchmark_rx(uint8_t source, uint32_t sequence, uint32_t gap,
                            uint32_t source_tick, uint8_t wire_type, uint8_t event_type,
                            uint8_t position, uint8_t pressed) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_RX source=%u seq=%u gap=%u source_tick=%u dongle_tick=%u wire=%u "
            "event=%u position=%u pressed=%u",
            source, sequence, gap, source_tick, k_cycle_get_32(), wire_type, event_type,
            position, pressed);
#else
    ARG_UNUSED(source);
    ARG_UNUSED(sequence);
    ARG_UNUSED(gap);
    ARG_UNUSED(source_tick);
    ARG_UNUSED(wire_type);
    ARG_UNUSED(event_type);
    ARG_UNUSED(position);
    ARG_UNUSED(pressed);
#endif
}

void totem_esb_benchmark_rx_invalid(uint8_t pipe, int error) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_RX_INVALID pipe=%u error=%d dongle_tick=%u", pipe, error, k_cycle_get_32());
#else
    ARG_UNUSED(pipe);
    ARG_UNUSED(error);
#endif
}

void totem_esb_benchmark_tx(uint16_t message_id, uint16_t attempts, bool success) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    LOG_INF("BENCH_TX msg=%u attempts=%u success=%u source_tick=%u", message_id, attempts,
            success, k_cycle_get_32());
#else
    ARG_UNUSED(message_id);
    ARG_UNUSED(attempts);
    ARG_UNUSED(success);
#endif
}
