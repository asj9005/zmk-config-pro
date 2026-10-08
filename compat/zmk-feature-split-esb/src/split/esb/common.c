/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "common.h"
#include "app_esb.h"

#include <stddef.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <totem/esb_benchmark.h>
#include <totem/esb_diagnostics.h>
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
#include <totem/esb_v3_crypto.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ESB_MAX_PAYLOAD_LENGTH <= UINT8_MAX,
             "Local RX metadata requires a one-byte radio packet length");

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
BUILD_ASSERT(offsetof(struct esb_command_payload, body) ==
                 sizeof(struct esb_v3_wire_payload_header),
             "Secure command header layout diverged from authenticated AAD");
BUILD_ASSERT(offsetof(struct esb_event_payload, body) ==
                 sizeof(struct esb_v3_wire_payload_header),
             "Secure event header layout diverged from authenticated AAD");
BUILD_ASSERT(sizeof(struct esb_msg_postfix) == TOTEM_ESB_V3_TAG_SIZE,
             "Secure ESB postfix must contain exactly one CCM tag");
#endif

void zmk_split_esb_tx(struct zmk_split_esb_state *state) {
    size_t tx_buf_len = ring_buf_size_get(state->tx_buf);
    if (tx_buf_len < sizeof(struct esb_msg_prefix)) {
        return;
    }

    struct esb_msg_prefix prefix;
    if (ring_buf_peek(state->tx_buf, (uint8_t *)&prefix, sizeof(prefix)) != sizeof(prefix)) {
        return;
    }
    if (memcmp(prefix.magic_prefix, ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
               sizeof(prefix.magic_prefix)) != 0) {
        LOG_ERR("Invalid frame at the head of the local ESB TX ring; resetting it");
        ring_buf_reset(state->tx_buf);
        return;
    }

    size_t radio_len = sizeof(prefix) + prefix.payload_size
#if ESB_MSG_HAS_POSTFIX
                       + sizeof(struct esb_msg_postfix)
#endif
        ;
    size_t frame_len = radio_len + sizeof(struct esb_msg_meta);
    if (radio_len > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        LOG_ERR("Local ESB frame is too large for radio payload (%u > %u)", radio_len,
                CONFIG_ESB_MAX_PAYLOAD_LENGTH);
        ring_buf_reset(state->tx_buf);
        return;
    }
    if (tx_buf_len < frame_len) {
        return;
    }

    uint8_t buf[CONFIG_ESB_MAX_PAYLOAD_LENGTH + sizeof(struct esb_msg_meta)];
    size_t claim_len = 0;
    while (claim_len < frame_len) {
        uint8_t *b;
        uint32_t buf_len = ring_buf_get_claim(state->tx_buf, &b, frame_len - claim_len);
        if (buf_len <= 0) {
            break;
        }
        memcpy(&buf[claim_len], b, buf_len);
        claim_len += buf_len;
    }
    if (claim_len != frame_len) {
        ring_buf_get_finish(state->tx_buf, 0);
        return;
    }

    struct esb_msg_meta meta;
    memcpy(&meta, &buf[radio_len], sizeof(meta));

    app_esb_data_t tx_data = {
        .pipe = meta.pipe,
        .data = buf,
        .len = radio_len,
        .msg_id = meta.msg_id,
        .max_retry = meta.max_retry
    };
    int err = zmk_split_esb_send(&tx_data); // callback > zmk_split_esb_cb()

    /*
     * Preserve the complete frame when the lower application queue is full.
     * A subsequent TX callback will retry the same head frame.
     */
    ring_buf_get_finish(state->tx_buf, err == 0 ? frame_len : 0);
}

void zmk_split_esb_cb(app_esb_event_t *event, struct zmk_split_esb_state *state) {
    switch(event->evt_type) {
        case APP_ESB_EVT_TX_SUCCESS:
            totem_esb_diag_event(TOTEM_DIAG_TX_OK, 0);
            totem_esb_benchmark_tx(state->tx_pipe > 0 ? state->tx_pipe - 1 : UINT8_MAX,
                                   event->msg_id, event->tx_attempts, true);
            // LOG_DBG("ESB TX sent");
            if (!ring_buf_is_empty(state->tx_buf)) {
                zmk_split_esb_tx(state);
            }
            break;
        case APP_ESB_EVT_TX_FAIL:
            totem_esb_diag_event(TOTEM_DIAG_TX_FAIL, 0);
            totem_esb_benchmark_tx(state->tx_pipe > 0 ? state->tx_pipe - 1 : UINT8_MAX,
                                   event->msg_id, event->tx_attempts, false);
            // LOG_WRN("ESB TX failed");
            if (!ring_buf_is_empty(state->tx_buf)) {
                zmk_split_esb_tx(state);
            }
            break;
        case APP_ESB_EVT_TX_SPACE_AVAILABLE:
            if (!ring_buf_is_empty(state->tx_buf)) {
                zmk_split_esb_tx(state);
            }
            break;
        case APP_ESB_EVT_RX:
            totem_esb_diag_event(TOTEM_DIAG_RX, 0);
            // LOG_DBG("ESB RX received: {%d} %d", event->pipe, event->data_length);
            if (event->pipe >= CONFIG_ESB_PIPE_COUNT ||
                event->pipe < state->rx_first_pipe ||
                event->pipe > state->rx_last_pipe) {
                LOG_ERR("Ignoring RX payload for invalid ESB pipe %u", event->pipe);
                totem_esb_benchmark_rx_invalid(event->pipe, -EADDRNOTAVAIL);
                break;
            }

            if (event->data_length == 0 ||
                event->data_length > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
                totem_esb_benchmark_rx_invalid(event->pipe, -EMSGSIZE);
                totem_esb_diag_event(TOTEM_DIAG_FRAME_ERR, -EMSGSIZE);
                break;
            }

            struct esb_rx_record record = {
                .received_at = k_uptime_get_32(),
                .pipe = event->pipe,
                .length = event->data_length,
            };
            size_t record_size = sizeof(record) + record.length;
            k_spinlock_key_t key = k_spin_lock(&state->rx_lock);
            struct ring_buf *rx_buf = state->rx_buf;
            if (ring_buf_space_get(rx_buf) < record_size ||
                state->rx_pipe_bytes[event->pipe] + record_size >
                    state->rx_pipe_capacity) {
                state->rx_overflow_count[event->pipe]++;
                uint32_t overflows = state->rx_overflow_count[event->pipe];
                k_spin_unlock(&state->rx_lock, key);
                totem_esb_benchmark_rx_overflow(
                    event->pipe, overflows);
                totem_esb_diag_event(TOTEM_DIAG_RX_OVERFLOW, event->pipe);
                break;
            }

            /* Commit the header and its complete packet together. No parser or
             * callback runs under this lock; the radio's reused buffer can be
             * overwritten as soon as the copies complete. */
            ring_buf_put(rx_buf, (const uint8_t *)&record, sizeof(record));
            ring_buf_put(rx_buf, event->buf, record.length);
            state->rx_pipe_bytes[event->pipe] += record_size;
            uint32_t queued_bytes = ring_buf_size_get(rx_buf);
            k_spin_unlock(&state->rx_lock, key);
            totem_esb_diag_rx_observe(queued_bytes, 0);

            if (state->process_rx_callback) {
                state->process_rx_callback(event->pipe);
            }

            break;
        default:
            LOG_ERR("Unknown APP ESB event!");
            break;
    }
}

int zmk_split_esb_finalize_item(uint8_t *env, size_t env_len,
                                bool downlink, struct esb_msg_postfix *postfix) {
    if (env == NULL || env_len < sizeof(struct esb_msg_prefix) || postfix == NULL) {
        return -EINVAL;
    }
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (env_len < sizeof(struct esb_msg_prefix) +
                      sizeof(struct esb_v3_wire_payload_header)) {
        return -EMSGSIZE;
    }
    struct esb_v3_wire_payload_header *header =
        (void *)(env + sizeof(struct esb_msg_prefix));
    if (header->source == 0 ||
        header->source >= CONFIG_ESB_PIPE_COUNT) {
        return -EADDRNOTAVAIL;
    }
    enum totem_esb_v3_key_stage stage = TOTEM_ESB_V3_ACTIVE_KEY;
    if ((!downlink &&
         (header->wire_type == ESB_WIRE_EVENT_V3_HELLO ||
          header->wire_type == ESB_WIRE_EVENT_V3_RECOVERY)) ||
        (downlink && header->wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE)) {
        stage = TOTEM_ESB_V3_ROOT_KEY;
    } else if ((!downlink && header->wire_type == ESB_WIRE_EVENT_V3_READY) ||
               (downlink &&
                header->wire_type == ESB_WIRE_COMMAND_V3_SESSION_OK)) {
        stage = TOTEM_ESB_V3_PENDING_KEY;
    }
    size_t aad_len =
        sizeof(struct esb_msg_prefix) + sizeof(struct esb_v3_wire_payload_header);
    size_t body_len = env_len - aad_len;
    return totem_esb_v3_seal(
        header->source - 1U,
        downlink ? TOTEM_ESB_V3_DOWNLINK : TOTEM_ESB_V3_UPLINK, stage,
        header->session_id, header->sequence, env, aad_len, env + aad_len,
        body_len, postfix->tag);
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
    postfix->crc = crc32_ieee(env, env_len);
    return 0;
#else
    ARG_UNUSED(downlink);
    return 0;
#endif
}

/* Count one parser outcome, including failures before authentication. */
static inline int diag_frame_result(int result) {
    totem_esb_diag_event(result == 0 ? TOTEM_DIAG_FRAME_OK : TOTEM_DIAG_FRAME_ERR,
                         result);
    return result;
}

/* Decode exactly one radio payload. A malformed packet cannot consume or reset
 * any subsequent admission, including one received from the other half. */
static int decode_rx_packet(const uint8_t *packet, size_t packet_size,
                            uint8_t *env, size_t env_size,
                            bool downlink, uint8_t expected_pipe) {
#if !IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    ARG_UNUSED(expected_pipe);
    ARG_UNUSED(downlink);
#endif
    if (packet_size <= ESB_MSG_WIRE_MIN_SIZE) {
        return diag_frame_result(-EMSGSIZE);
    }
    struct esb_msg_prefix prefix;
    memcpy(&prefix, packet, sizeof(prefix));

    if (memcmp(&prefix.magic_prefix, &ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
               sizeof(prefix.magic_prefix)) != 0) {
        return diag_frame_result(-EPROTO);
    }

    size_t payload_to_read = sizeof(prefix) + prefix.payload_size;

    if (payload_to_read > env_size) {
        LOG_WRN("Invalid message with payload %d bigger than expected max %d",
            payload_to_read, env_size);
        return diag_frame_result(-EMSGSIZE);
    }

    if (packet_size != (payload_to_read
#if ESB_MSG_HAS_POSTFIX
        + sizeof(struct esb_msg_postfix)
#endif
    )) {
        return diag_frame_result(-EMSGSIZE);
    }

    memcpy(env, packet, payload_to_read);

#if ESB_MSG_HAS_POSTFIX
    struct esb_msg_postfix postfix;
    memcpy(&postfix, packet + payload_to_read, sizeof(postfix));

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (payload_to_read <
        sizeof(struct esb_msg_prefix) +
            sizeof(struct esb_v3_wire_payload_header)) {
        return diag_frame_result(-EMSGSIZE);
    }
    struct esb_v3_wire_payload_header *header =
        (void *)(env + sizeof(struct esb_msg_prefix));
    if (header->source == 0 ||
        header->source >= CONFIG_ESB_PIPE_COUNT ||
        header->source != expected_pipe) {
        return diag_frame_result(-EADDRNOTAVAIL);
    }
    enum totem_esb_v3_key_stage stage = TOTEM_ESB_V3_ACTIVE_KEY;
    if ((!downlink &&
         (header->wire_type == ESB_WIRE_EVENT_V3_HELLO ||
          header->wire_type == ESB_WIRE_EVENT_V3_RECOVERY)) ||
        (downlink &&
         header->wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE)) {
        stage = TOTEM_ESB_V3_ROOT_KEY;
    } else if ((!downlink &&
                header->wire_type == ESB_WIRE_EVENT_V3_READY) ||
               (downlink &&
                header->wire_type == ESB_WIRE_COMMAND_V3_SESSION_OK)) {
        stage = TOTEM_ESB_V3_PENDING_KEY;
    }
    size_t aad_len = sizeof(struct esb_msg_prefix) +
                     sizeof(struct esb_v3_wire_payload_header);
    size_t body_len = payload_to_read - aad_len;
    int crypto_err = totem_esb_v3_open(
        header->source - 1U,
        downlink ? TOTEM_ESB_V3_DOWNLINK : TOTEM_ESB_V3_UPLINK, stage,
        header->session_id, header->sequence, env, aad_len, env + aad_len,
        body_len, postfix.tag);
    if (crypto_err != 0) {
        totem_esb_benchmark_security_drop(
            header->source - 1U,
            crypto_err == -EACCES ? "auth" : "session", header->sequence);
        return diag_frame_result(crypto_err);
    }
#else
    uint32_t crc = crc32_ieee(env, payload_to_read);

    if (crc != postfix.crc) {
        LOG_WRN("Data corruption in received peripheral event (%d vs %d)",
                crc, postfix.crc);
        return diag_frame_result(-EBADMSG);
    }
#endif
#endif

    return diag_frame_result(0);
}

/* A queued record is necessarily from the current uptime epoch. Unsigned
 * subtraction expands its 32-bit clock across wrap without changing the wire
 * format. The bounded RX FIFO cannot legitimately retain traffic for 49 days. */
static int64_t rx_record_timestamp(uint32_t received_at) {
    int64_t now = k_uptime_get();
    return now - (uint32_t)((uint32_t)now - received_at);
}

static bool rx_record_peek_valid(struct zmk_split_esb_state *state,
                                  struct esb_rx_record *record) {
    struct ring_buf *rx_buf = state->rx_buf;
    return ring_buf_peek(rx_buf, (uint8_t *)record, sizeof(*record)) == sizeof(*record) &&
        record->pipe < CONFIG_ESB_PIPE_COUNT &&
        record->pipe >= state->rx_first_pipe && record->pipe <= state->rx_last_pipe &&
        record->length != 0 && record->length <= CONFIG_ESB_MAX_PAYLOAD_LENGTH &&
        ring_buf_size_get(rx_buf) >= sizeof(*record) + record->length &&
        state->rx_pipe_bytes[record->pipe] >= sizeof(*record) + record->length;
}

static void rx_record_reset(struct zmk_split_esb_state *state) {
    ring_buf_reset(state->rx_buf);
    memset(state->rx_pipe_bytes, 0, sizeof(state->rx_pipe_bytes));
}

bool zmk_split_esb_rx_pending_before(struct zmk_split_esb_state *state,
                                      int64_t deadline) {
    struct esb_rx_record record;
    k_spinlock_key_t key = k_spin_lock(&state->rx_lock);
    if (ring_buf_is_empty(state->rx_buf)) {
        k_spin_unlock(&state->rx_lock, key);
        return false;
    }
    bool valid = rx_record_peek_valid(state, &record);
    if (!valid) {
        /* Match dequeue recovery; corrupt local metadata must not indefinitely
         * defer a timer. A malformed radio payload is still dequeued normally. */
        rx_record_reset(state);
    }
    k_spin_unlock(&state->rx_lock, key);
    if (!valid) {
        (void)diag_frame_result(-EIO);
        return false;
    }
    return rx_record_timestamp(record.received_at) <= deadline;
}

int zmk_split_esb_rx_get(struct zmk_split_esb_state *state, uint8_t *env,
                         size_t env_size, bool downlink, uint8_t *pipe,
                         int64_t *received_at) {
    struct esb_rx_record record;
    uint8_t packet[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    k_spinlock_key_t key = k_spin_lock(&state->rx_lock);
    struct ring_buf *rx_buf = state->rx_buf;
    if (ring_buf_is_empty(rx_buf)) {
        k_spin_unlock(&state->rx_lock, key);
        return -ENODATA;
    }
    /* These metadata bytes are generated locally, never supplied by a peer.
     * Protect the queue against an internal invariant failure as well. */
    if (!rx_record_peek_valid(state, &record)) {
        rx_record_reset(state);
        k_spin_unlock(&state->rx_lock, key);
        return diag_frame_result(-EIO);
    }
    ring_buf_get(rx_buf, (uint8_t *)&record, sizeof(record));
    ring_buf_get(rx_buf, packet, record.length);
    state->rx_pipe_bytes[record.pipe] -= sizeof(record) + record.length;
    uint32_t queued_bytes = ring_buf_size_get(rx_buf);
    k_spin_unlock(&state->rx_lock, key);

    *pipe = record.pipe;
    if (received_at != NULL) {
        *received_at = rx_record_timestamp(record.received_at);
    }
    totem_esb_diag_rx_observe(queued_bytes, k_uptime_get_32() - record.received_at);
    return decode_rx_packet(packet, record.length, env, env_size, downlink, *pipe);
}
