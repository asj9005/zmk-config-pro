/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/ring_buffer.h>
#include <zephyr/device.h>

#include <zmk/split/transport/types.h>
#include <totem/esb_benchmark.h>
#include "app_esb.h"

#define ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX "ZmKe"

struct esb_msg_prefix {
    uint8_t magic_prefix[sizeof(ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX) - 1];
    uint8_t payload_size;
} __packed;

struct esb_command_payload {
    uint8_t source;
    struct zmk_split_transport_central_command cmd;
} __packed;

struct esb_command_envelope {
    struct esb_msg_prefix prefix;
    struct esb_command_payload payload;
} __packed;

enum esb_wire_event_type {
    ESB_WIRE_EVENT_ZMK = 0,
    ESB_WIRE_EVENT_HEARTBEAT,
    ESB_WIRE_EVENT_BENCHMARK,
};

struct esb_event_payload {
    uint8_t source;
    uint8_t wire_type;
    uint32_t sequence;
    uint32_t session_id;
    uint32_t source_tick;
    union {
        struct zmk_split_transport_peripheral_event event;
        struct totem_esb_link_metric_payload link_metric;
    } body;
} __packed;

struct esb_event_envelope {
    struct esb_msg_prefix prefix;
    struct esb_event_payload payload;
} __packed;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
struct esb_msg_postfix {
    uint32_t crc;
} __packed;
#endif

struct esb_msg_meta {
    uint16_t msg_id;
    uint8_t max_retry;
    uint8_t pipe;
} __packed;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
#define ESB_MSG_WIRE_MIN_SIZE (sizeof(struct esb_msg_prefix) + sizeof(struct esb_msg_postfix))
#define ESB_MSG_EXTRA_SIZE (ESB_MSG_WIRE_MIN_SIZE + sizeof(struct esb_msg_meta))
#else
#define ESB_MSG_WIRE_MIN_SIZE sizeof(struct esb_msg_prefix)
#define ESB_MSG_EXTRA_SIZE (ESB_MSG_WIRE_MIN_SIZE + sizeof(struct esb_msg_meta))
#endif

typedef void (*zmk_split_esb_process_rx_callback_t)(uint8_t pipe);

struct zmk_split_esb_state {
    uint8_t tx_pipe;
    zmk_split_esb_process_rx_callback_t process_rx_callback;
    struct ring_buf *tx_buf;
    struct ring_buf *rx_bufs;
    uint32_t rx_overflow_count[CONFIG_ESB_PIPE_COUNT];
};

void zmk_split_esb_tx(struct zmk_split_esb_state *state);

void zmk_split_esb_cb(app_esb_event_t *event, struct zmk_split_esb_state *state);

int zmk_split_esb_get_item(struct ring_buf *rx_buf, uint8_t *env, size_t env_size);
