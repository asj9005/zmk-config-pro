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

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
#define ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX "ZmK3"
#else
#define ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX "ZmKe"
#endif

struct esb_msg_prefix {
    uint8_t magic_prefix[sizeof(ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX) - 1];
    uint8_t payload_size;
} __packed;

struct esb_command_payload {
    uint8_t source;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    uint8_t wire_type;
    uint32_t sequence;
    uint64_t session_id;
    uint32_t source_tick;
    union {
        struct zmk_split_transport_central_command cmd;
        struct {
            uint64_t peripheral_nonce;
            uint32_t request_sequence;
            uint64_t reset_session;
        } __packed challenge;
    } body;
#else
    struct zmk_split_transport_central_command cmd;
#endif
} __packed;

struct esb_command_envelope {
    struct esb_msg_prefix prefix;
    struct esb_command_payload payload;
} __packed;

enum esb_wire_event_type {
    ESB_WIRE_EVENT_ZMK = 0,
    ESB_WIRE_EVENT_HEARTBEAT,
    ESB_WIRE_EVENT_BENCHMARK,
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    ESB_WIRE_EVENT_V3_HELLO,
    ESB_WIRE_EVENT_V3_RECOVERY,
    ESB_WIRE_EVENT_V3_READY,
#endif
    /* Keep every existing wire value; snapshots use the same value in v2/v3. */
    ESB_WIRE_EVENT_KEY_STATE = 6,
};

#define ESB_KEY_STATE_BYTES ((CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX + 7) / 8)

struct esb_key_state_payload {
    uint8_t keys[ESB_KEY_STATE_BYTES];
} __packed;

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
enum esb_wire_command_type {
    ESB_WIRE_COMMAND_ZMK = 0,
    ESB_WIRE_COMMAND_V3_CHALLENGE,
    ESB_WIRE_COMMAND_V3_SESSION_OK,
};

struct esb_v3_wire_payload_header {
    uint8_t source;
    uint8_t wire_type;
    uint32_t sequence;
    uint64_t session_id;
    uint32_t source_tick;
} __packed;

struct esb_v3_recovery_payload {
    uint64_t active_session;
    struct totem_esb_link_metric_payload link_metric;
} __packed;
#endif

struct esb_event_payload {
    uint8_t source;
    uint8_t wire_type;
    uint32_t sequence;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    uint64_t session_id;
#else
    uint32_t session_id;
#endif
    uint32_t source_tick;
    union {
        struct zmk_split_transport_peripheral_event event;
        struct totem_esb_link_metric_payload link_metric;
        struct esb_key_state_payload key_state;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
        struct esb_v3_recovery_payload recovery;
#endif
    } body;
} __packed;

struct esb_event_envelope {
    struct esb_msg_prefix prefix;
    struct esb_event_payload payload;
} __packed;

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
struct esb_msg_postfix {
    uint8_t tag[4];
} __packed;
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
struct esb_msg_postfix {
    uint32_t crc;
} __packed;
#endif

struct esb_msg_meta {
    uint16_t msg_id;
    uint8_t max_retry;
    uint8_t pipe;
} __packed;

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3) || IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
#define ESB_MSG_HAS_POSTFIX 1
#define ESB_MSG_WIRE_MIN_SIZE (sizeof(struct esb_msg_prefix) + sizeof(struct esb_msg_postfix))
#define ESB_MSG_EXTRA_SIZE (ESB_MSG_WIRE_MIN_SIZE + sizeof(struct esb_msg_meta))
#else
#define ESB_MSG_HAS_POSTFIX 0
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

int zmk_split_esb_finalize_item(uint8_t *env, size_t env_len,
                                bool downlink, struct esb_msg_postfix *postfix);
int zmk_split_esb_get_item(struct ring_buf *rx_buf, uint8_t *env, size_t env_size,
                           bool downlink, uint8_t expected_pipe);
