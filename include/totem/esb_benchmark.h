/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

enum totem_esb_link_metric {
    TOTEM_ESB_LINK_METRIC_TX_MESSAGES = 0,
    TOTEM_ESB_LINK_METRIC_TX_ATTEMPTS,
    TOTEM_ESB_LINK_METRIC_TX_FAILURES,
    TOTEM_ESB_LINK_METRIC_APP_QUEUE_PRESSURE,
    TOTEM_ESB_LINK_METRIC_PRODUCER_QUEUE_OVERFLOW,
    TOTEM_ESB_LINK_METRIC_COUNT,
};

struct totem_esb_link_metric_payload {
    uint8_t metric;
    uint32_t value;
} __packed;

void totem_esb_benchmark_rx(uint8_t source, uint32_t session_id, uint32_t sequence,
                            uint32_t gap, uint32_t source_tick, uint8_t wire_type,
                            uint8_t event_type, uint8_t position, uint8_t pressed,
                            bool accepted_for_zmk);
void totem_esb_benchmark_rx_invalid(uint8_t pipe, int error);
void totem_esb_benchmark_rx_overflow(uint8_t pipe, uint32_t count);
void totem_esb_benchmark_link_metric(uint8_t source, uint32_t session_id, uint8_t metric,
                                     uint32_t value);
void totem_esb_benchmark_tx(uint8_t source, uint16_t message_id, uint16_t attempts,
                            bool success);
void totem_esb_transport_queue_pressure(bool producer_ring);
uint32_t totem_esb_link_metric_value(uint8_t metric);
void totem_esb_peer_seen(uint8_t source);
bool totem_esb_peer_is_connected(uint8_t source);
uint8_t totem_esb_peer_connected_count(void);
void totem_esb_notify_transport_status(void);
void totem_esb_source_disconnected(uint8_t source);
void totem_esb_schedule_display_sync(void);
