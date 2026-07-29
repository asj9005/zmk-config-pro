/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "common.h"
#include "app_esb.h"

#include <zephyr/sys/crc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <totem/esb_benchmark.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

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
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
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
            totem_esb_benchmark_tx(state->tx_pipe > 0 ? state->tx_pipe - 1 : UINT8_MAX,
                                   event->msg_id, event->tx_attempts, true);
            // LOG_DBG("ESB TX sent");
            if (!ring_buf_is_empty(state->tx_buf)) {
                zmk_split_esb_tx(state);
            }
            break;
        case APP_ESB_EVT_TX_FAIL:
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
            // LOG_DBG("ESB RX received: {%d} %d", event->pipe, event->data_length);
            if (event->pipe >= CONFIG_ESB_PIPE_COUNT) {
                LOG_ERR("Ignoring RX payload for invalid ESB pipe %u", event->pipe);
                totem_esb_benchmark_rx_invalid(event->pipe, -EADDRNOTAVAIL);
                break;
            }

            struct ring_buf *rx_buf = &state->rx_bufs[event->pipe];

            if (ring_buf_space_get(rx_buf) < event->data_length) {
                state->rx_overflow_count[event->pipe]++;
                totem_esb_benchmark_rx_overflow(
                    event->pipe, state->rx_overflow_count[event->pipe]);
                LOG_WRN("No room to receive (have %d but only space for %d/%d)",
                        event->data_length, ring_buf_space_get(rx_buf),
                        ring_buf_capacity_get(rx_buf));
                break;
            }

            size_t received = ring_buf_put(rx_buf, event->buf, event->data_length);
            if (received < event->data_length) {
                state->rx_overflow_count[event->pipe]++;
                totem_esb_benchmark_rx_overflow(
                    event->pipe, state->rx_overflow_count[event->pipe]);
                LOG_ERR("RX overrun! %d < %d", received, event->data_length);
                break;
            }

            // LOG_DBG("RX + %3d and now buffer is %3d", received, ring_buf_size_get(&rx_buf));
            if (state->process_rx_callback) {
                state->process_rx_callback(event->pipe);
            }

            break;
        default:
            LOG_ERR("Unknown APP ESB event!");
            break;
    }
}

int zmk_split_esb_get_item(struct ring_buf *rx_buf, uint8_t *env, size_t env_size) {
    // RX buffer only has prefix + postfix
    while (ring_buf_size_get(rx_buf) > (sizeof(struct esb_msg_prefix)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
            + sizeof(struct esb_msg_postfix)
#endif
    )) {
        struct esb_msg_prefix prefix;

        __ASSERT_EVAL(
            (void)ring_buf_peek(rx_buf, (uint8_t *)&prefix, sizeof(prefix)),
            uint32_t peek_read = ring_buf_peek(rx_buf, (uint8_t *)&prefix, sizeof(prefix)),
            peek_read == sizeof(prefix), "Somehow read less than we expect from the RX buffer");

        if (memcmp(&prefix.magic_prefix, &ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                   sizeof(prefix.magic_prefix)) != 0) {
            LOG_WRN("Multiple prefix mismatches, resetting buffer");
            ring_buf_reset(rx_buf);

            // // LOG_WRN("Prefix mismatch, skipping 1 byte to realign");
            // // Drop a single byte to let the stream re-align
            // uint8_t dummy;
            // ring_buf_get(rx_buf, &dummy, 1);

            return -EPROTO;
        }

        size_t payload_to_read = sizeof(prefix) + prefix.payload_size;

        if (payload_to_read > env_size) {
            LOG_WRN("Invalid message with payload %d bigger than expected max %d",
                payload_to_read, env_size);
            ring_buf_reset(rx_buf);
            return -EMSGSIZE;
        }

        if (ring_buf_size_get(rx_buf) < (payload_to_read
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
            + sizeof(struct esb_msg_postfix)
#endif
        )) {
            return -EAGAIN;
        }

        // Now that prefix matches, read it out so we can read the rest of the payload.
        __ASSERT_EVAL((void)ring_buf_get(rx_buf, env, payload_to_read),
                      uint32_t read = ring_buf_get(rx_buf, env, payload_to_read),
                      read == payload_to_read,
                      "Somehow read less than we expect from the RX buffer");

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
        struct esb_msg_postfix postfix;
        __ASSERT_EVAL((void)ring_buf_get(rx_buf, (uint8_t *)&postfix, sizeof(postfix)),
                      uint32_t read = ring_buf_get(rx_buf, (uint8_t *)&postfix, sizeof(postfix)),
                      read == sizeof(postfix),
                      "Somehow read less of the postfix than we expect from the RX buffer");

        // LOG_HEXDUMP_DBG(&postfix, sizeof(postfix), "postfix");

        uint32_t crc = crc32_ieee(env, payload_to_read);

        if (crc != postfix.crc) {
            LOG_WRN("Data corruption in received peripheral event, resetting buffer (%d vs %d)",
                    crc, postfix.crc);
            return -EBADMSG;
        }
#endif

        return 0;
    }

    return -EAGAIN;
}
