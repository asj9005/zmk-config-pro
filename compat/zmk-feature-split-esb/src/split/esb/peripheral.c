/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

#if IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#endif
#if IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTICS)
#include <zephyr/sys/printk.h>
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#include <zmk/stdlib.h>
#include <zmk/behavior.h>
#include <zmk/sensors.h>
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/pointing/input_split.h>
#include <zmk/hid_indicators_types.h>
#include <zmk/physical_layouts.h>

#include <totem/esb_benchmark.h>
#include <totem/esb_diagnostics.h>
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
#include <totem/esb_backoff.h>
#include <totem/esb_v3_crypto.h>
#endif

#include "app_esb.h"
#include "common.h"

#if ESB_MSG_HAS_POSTFIX
#define TX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_postfix) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_postfix))
#else
#define TX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_command_envelope))
#endif

BUILD_ASSERT(TX_BUFFER_SIZE - sizeof(struct esb_msg_meta) <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB peripheral event plus local metadata exceeds the configured payload");
BUILD_ASSERT(RX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB central command exceeds the configured payload");

RING_BUF_DECLARE(tx_buf, TX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS);
static struct k_spinlock tx_ring_lock;

/* Serialize physical key updates with edge/snapshot commits to the TX FIFO. */
K_MUTEX_DEFINE(event_mutex);
static struct esb_key_state_payload local_key_state;
static void key_state_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(key_state_work, key_state_work_cb);

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
static atomic_t transport_ready;
#endif

#define RX_RING_BUF_SIZE (RX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_CMD_BUFFER_ITEMS)
struct ring_buf rx_bufs[CONFIG_ESB_PIPE_COUNT];
uint8_t rx_bufs_data[CONFIG_ESB_PIPE_COUNT][RX_RING_BUF_SIZE];

static const uint8_t peripheral_id = CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID;
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID > 0,
             "Pipe 0 is reserved; peripheral IDs start at 1");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID < CONFIG_ESB_PIPE_COUNT,
             "Peripheral ID must map to a configured ESB pipe");

static void process_rx_cb(uint8_t pipe);

static struct zmk_split_esb_state state = {
    .tx_pipe = CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID % CONFIG_ESB_PIPE_COUNT,
    .process_rx_callback = process_rx_cb,
    .tx_buf = &tx_buf,
    .rx_bufs = rx_bufs,
};

static void begin_tx(void) {
    zmk_split_esb_tx(&state);
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
static void handshake_radio_ack_cb(void);
#endif

void zmk_split_esb_on_ptx_esb_callback(app_esb_event_t *event) {
    zmk_split_esb_cb(event, &state);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (event->evt_type == APP_ESB_EVT_TX_SUCCESS) {
        handshake_radio_ack_cb();
    }
#endif
}

static ssize_t get_payload_data_size(const struct zmk_split_transport_peripheral_event *evt) {
    switch (evt->type) {
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT:
        return sizeof(evt->data.input_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT:
        return sizeof(evt->data.key_position_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT:
        return sizeof(evt->data.sensor_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT:
        return sizeof(evt->data.battery_event);
    default:
        return -ENOTSUP;
    }
}

static uint8_t get_retry_count(const struct zmk_split_transport_peripheral_event *evt) {
    switch (evt->type) {
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT:
        return CONFIG_ZMK_SPLIT_ESB_RETRY_INPUT_EVENT;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT:
        return CONFIG_ZMK_SPLIT_ESB_RETRY_KEY_POSITION;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT:
        return CONFIG_ZMK_SPLIT_ESB_RETRY_SENSOR_EVENT;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT:
        return CONFIG_ZMK_SPLIT_ESB_RETRY_BATTERY_EVENT;
    default:
        return 0;
    }
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
BUILD_ASSERT(CONFIG_TOTEM_ESB_V3_HANDSHAKE_MAX_INTERVAL_MS >=
                 CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS,
             "Handshake backoff maximum must cover the initial probe interval");

enum esb_v3_peripheral_state {
    ESB_V3_NO_SESSION = 0,
    ESB_V3_WAIT_CHALLENGE,
    ESB_V3_WAIT_SESSION_OK,
    ESB_V3_ESTABLISHED,
};

static atomic_t secure_state = ATOMIC_INIT(ESB_V3_NO_SESSION);
static uint64_t peripheral_nonce;
static uint64_t central_nonce;
static uint64_t wire_session_id;
static uint32_t wire_sequence;
static uint32_t root_sequence;
static uint32_t last_probe_sequence;
static uint32_t accepted_challenge_request = UINT32_MAX;
static uint32_t downlink_sequence;
static int64_t wait_session_ok_started_at;
static uint8_t heartbeat_metric;
static struct totem_esb_backoff handshake_backoff = {
    .next_ms = CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS,
};
static atomic_t handshake_radio_ack;
K_MSGQ_DEFINE(presession_events,
              sizeof(struct zmk_split_transport_peripheral_event),
              CONFIG_TOTEM_ESB_V3_PRESESSION_EVENT_QUEUE_SIZE, 4);
#define ESB_V3_PRESESSION_FLUSH_BATCH 4U
static void handshake_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(handshake_work, handshake_work_cb);
static void handshake_wakeup_work_cb(struct k_work *work);
static K_WORK_DEFINE(handshake_wakeup_work, handshake_wakeup_work_cb);
static void flush_presession_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(flush_presession_work, flush_presession_work_cb);

static enum esb_v3_peripheral_state get_secure_state(void) {
    return (enum esb_v3_peripheral_state)atomic_get(&secure_state);
}

static void set_secure_state(enum esb_v3_peripheral_state next) {
    atomic_set(&secure_state, next);
}

static void handshake_radio_ack_cb(void) {
    if (get_secure_state() != ESB_V3_ESTABLISHED) {
        atomic_set(&handshake_radio_ack, 1);
        /* IRQ-safe: never take event_mutex from the ESB radio callback. */
        k_work_submit(&handshake_wakeup_work);
    }
}

static void handshake_wakeup_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&event_mutex, K_FOREVER);
    if (atomic_set(&handshake_radio_ack, 0) != 0 &&
        get_secure_state() != ESB_V3_ESTABLISHED) {
        /* Pick up the dongle's next ACK payload before its queue entry expires. */
        totem_esb_backoff_reset(&handshake_backoff,
                                CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
        k_work_reschedule(&handshake_work, K_NO_WAIT);
    }
    k_mutex_unlock(&event_mutex);
}

static void schedule_handshake_retry(void) {
    k_mutex_lock(&event_mutex, K_FOREVER);
    uint32_t delay_ms = totem_esb_backoff_take(
        &handshake_backoff, CONFIG_TOTEM_ESB_V3_HANDSHAKE_MAX_INTERVAL_MS);
    k_work_reschedule(&handshake_work, K_MSEC(delay_ms));
    k_mutex_unlock(&event_mutex);
}

static void clear_v3_traffic_session_locked(void) {
    totem_esb_v3_discard_pending(peripheral_id - 1U);
    totem_esb_v3_clear_active(peripheral_id - 1U);
    central_nonce = 0;
    wire_session_id = 0;
    wire_sequence = 0;
    accepted_challenge_request = UINT32_MAX;
    downlink_sequence = 0;
    wait_session_ok_started_at = 0;
}

static void reset_v3_session_locked(enum esb_v3_peripheral_state next,
                                    uint64_t next_peripheral_nonce) {
    set_secure_state(next);
    clear_v3_traffic_session_locked();
    peripheral_nonce = next_peripheral_nonce;
    root_sequence = 0;
    last_probe_sequence = 0;
    totem_esb_backoff_reset(&handshake_backoff,
                            CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
    atomic_clear(&handshake_radio_ack);
}

static int begin_fresh_v3_handshake(void) {
    uint64_t fresh_nonce;
    int err = sys_csrand_get(&fresh_nonce, sizeof(fresh_nonce));
    if (err != 0 || fresh_nonce == 0) {
        return err != 0 ? err : -EIO;
    }

    /*
     * Finish any event already being sealed, then make the state transition
     * and nonce update indivisible from the perspective of later producers.
     * This mutex is only taken on boot/rekey, not for each state read.
     */
    k_mutex_lock(&event_mutex, K_FOREVER);
    reset_v3_session_locked(ESB_V3_WAIT_CHALLENGE, fresh_nonce);
    k_mutex_unlock(&event_mutex);
    return 0;
}

static void restart_v3_after_auth_failure(void) {
    /*
     * Fail closed before asking the RNG for a replacement nonce. If entropy
     * is temporarily unavailable, handshake_work retries while NO_SESSION
     * prevents any old-key traffic from being accepted or produced.
     */
    k_mutex_lock(&event_mutex, K_FOREVER);
    reset_v3_session_locked(ESB_V3_NO_SESSION, 0);
    k_mutex_unlock(&event_mutex);

    int err = begin_fresh_v3_handshake();
    if (err != 0) {
        LOG_ERR("ESB v3 auth-failure rekey nonce generation failed (%d)", err);
        schedule_handshake_retry();
        return;
    }
    k_work_reschedule(&handshake_work, K_NO_WAIT);
}

static int enqueue_v3_frame(
    enum esb_wire_event_type wire_type,
    const struct zmk_split_transport_peripheral_event *event) {
    ssize_t data_size = 0;
    size_t body_size = 0;
    if (wire_type == ESB_WIRE_EVENT_ZMK) {
        if (event == NULL) {
            return -EINVAL;
        }
        data_size = get_payload_data_size(event);
        if (data_size < 0) {
            return data_size;
        }
        body_size = sizeof(event->type) + data_size;
    } else if (wire_type == ESB_WIRE_EVENT_V3_RECOVERY) {
        body_size = sizeof(struct esb_v3_recovery_payload);
    } else if (wire_type == ESB_WIRE_EVENT_KEY_STATE) {
        body_size = sizeof(struct esb_key_state_payload);
    } else if (wire_type != ESB_WIRE_EVENT_BENCHMARK &&
               wire_type != ESB_WIRE_EVENT_V3_HELLO &&
               wire_type != ESB_WIRE_EVENT_V3_READY) {
        return -ENOTSUP;
    }

    k_mutex_lock(&event_mutex, K_FOREVER);
    uint32_t sequence;
    uint64_t session_id;
    bool commit_recovery_sequence = false;
    bool commit_traffic_sequence = false;
    if (wire_type == ESB_WIRE_EVENT_V3_HELLO) {
        sequence = 0;
        session_id = peripheral_nonce;
    } else if (wire_type == ESB_WIRE_EVENT_V3_RECOVERY) {
        if (root_sequence >= UINT32_MAX - 1U) {
            k_mutex_unlock(&event_mutex);
            return -EOVERFLOW;
        }
        sequence = root_sequence + 1U;
        commit_recovery_sequence = true;
        session_id = peripheral_nonce;
    } else if (wire_type == ESB_WIRE_EVENT_V3_READY) {
        sequence = 0;
        session_id = wire_session_id;
    } else {
        if (get_secure_state() != ESB_V3_ESTABLISHED) {
            k_mutex_unlock(&event_mutex);
            return -ENOTCONN;
        }
        if (wire_sequence >= UINT32_MAX - 1U) {
            k_mutex_unlock(&event_mutex);
            return -EOVERFLOW;
        }
        sequence = wire_sequence + 1U;
        commit_traffic_sequence = true;
        session_id = wire_session_id;
    }

    size_t header_size = offsetof(struct esb_event_payload, body);
    size_t payload_size = header_size + body_size;
    struct esb_event_envelope env = {
        .prefix =
            {
                .magic_prefix = ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                .payload_size = payload_size,
            },
        .payload =
            {
                .source = peripheral_id,
                .wire_type = wire_type,
                .sequence = sequence,
                .session_id = session_id,
                .source_tick =
                    (wire_type == ESB_WIRE_EVENT_V3_HELLO ||
                     wire_type == ESB_WIRE_EVENT_V3_READY)
                        ? 0
                        : k_cycle_get_32(),
            },
    };
    if (wire_type == ESB_WIRE_EVENT_ZMK) {
        env.payload.body.event = *event;
    } else if (wire_type == ESB_WIRE_EVENT_KEY_STATE) {
        env.payload.body.key_state = local_key_state;
    } else if (wire_type == ESB_WIRE_EVENT_V3_RECOVERY) {
        env.payload.body.recovery.active_session =
            get_secure_state() == ESB_V3_ESTABLISHED ? wire_session_id : 0;
        env.payload.body.recovery.link_metric.metric = heartbeat_metric;
        env.payload.body.recovery.link_metric.value =
            totem_esb_link_metric_value(heartbeat_metric);
    }

    size_t env_len = sizeof(env.prefix) + payload_size;
    struct esb_msg_postfix postfix;
    int err =
        zmk_split_esb_finalize_item((uint8_t *)&env, env_len, false, &postfix);
    if (err != 0) {
        k_mutex_unlock(&event_mutex);
        return err;
    }

    size_t frame_size = env_len + sizeof(postfix) + sizeof(struct esb_msg_meta);
    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);
    if (ring_buf_space_get(&tx_buf) < frame_size) {
        totem_esb_transport_queue_pressure(true);
        k_spin_unlock(&tx_ring_lock, key);
        k_mutex_unlock(&event_mutex);
        return -ENOSPC;
    }
    size_t put = ring_buf_put(&tx_buf, (uint8_t *)&env, env_len);
    put += ring_buf_put(&tx_buf, (uint8_t *)&postfix, sizeof(postfix));
    static uint16_t event_message_id;
    if (++event_message_id >= UINT16_MAX - 1000U) {
        event_message_id = 1;
    }
    uint8_t max_retry =
        event != NULL
            ? get_retry_count(event)
            : (wire_type == ESB_WIRE_EVENT_KEY_STATE
                   ? CONFIG_ZMK_SPLIT_ESB_RETRY_KEY_POSITION
                   : ((wire_type == ESB_WIRE_EVENT_V3_HELLO ||
                       wire_type == ESB_WIRE_EVENT_V3_READY)
                          ? 3
                          : 1));
    struct esb_msg_meta meta = {
        .msg_id = event_message_id,
        .max_retry = max_retry,
        .pipe = state.tx_pipe,
    };
    put += ring_buf_put(&tx_buf, (uint8_t *)&meta, sizeof(meta));
    if (put != frame_size) {
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        k_mutex_unlock(&event_mutex);
        return -ENOSPC;
    }
    /*
     * A sequence becomes live only when the complete encrypted frame and its
     * retry metadata have been committed to the producer ring. Retrying an
     * ENOSPC frame therefore neither creates a false loss gap nor consumes a
     * CCM nonce for a packet that never left this function.
     */
    if (commit_recovery_sequence) {
        root_sequence = sequence;
        last_probe_sequence = sequence;
        heartbeat_metric =
            (heartbeat_metric + 1U) % TOTEM_ESB_LINK_METRIC_COUNT;
    } else if (commit_traffic_sequence) {
        wire_sequence = sequence;
    }
    begin_tx();
    k_spin_unlock(&tx_ring_lock, key);
    k_mutex_unlock(&event_mutex);
    return 0;
}

static int enqueue_wire_event(
    enum esb_wire_event_type wire_type,
    const struct zmk_split_transport_peripheral_event *event) {
    return enqueue_v3_frame(wire_type, event);
}
#else
static uint32_t wire_sequence;
static uint32_t wire_session_id;
static uint8_t heartbeat_metric;

static int enqueue_wire_event(enum esb_wire_event_type wire_type,
                              const struct zmk_split_transport_peripheral_event *event) {
    ssize_t data_size = 0;
    if (wire_type == ESB_WIRE_EVENT_ZMK) {
        data_size = get_payload_data_size(event);
        if (data_size < 0) {
            LOG_WRN("Failed to determine payload data size %d", data_size);
            return data_size;
        }
    }

    size_t payload_size = sizeof(peripheral_id) + sizeof(uint8_t) + sizeof(uint32_t) * 3;
    if (wire_type == ESB_WIRE_EVENT_ZMK) {
        payload_size += data_size + sizeof(enum zmk_split_transport_peripheral_event_type);
    } else if (wire_type == ESB_WIRE_EVENT_HEARTBEAT) {
        payload_size += sizeof(struct totem_esb_link_metric_payload);
    } else if (wire_type == ESB_WIRE_EVENT_KEY_STATE) {
        payload_size += sizeof(struct esb_key_state_payload);
    }

    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);

    if (ring_buf_space_get(&tx_buf) < ESB_MSG_EXTRA_SIZE + payload_size) {
        LOG_WRN("No room to send event to the central (have %d but only space for %d/%d)",
                ESB_MSG_EXTRA_SIZE + payload_size, ring_buf_space_get(&tx_buf),
                ring_buf_capacity_get(&tx_buf));
        totem_esb_transport_queue_pressure(true);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }

    struct esb_event_envelope env = {
        .prefix =
            {
                .magic_prefix = ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                .payload_size = payload_size,
            },
        .payload =
            {
                .source = peripheral_id,
                .wire_type = wire_type,
                .sequence = ++wire_sequence,
                .session_id = wire_session_id,
                .source_tick = k_cycle_get_32(),
            },
    };
    if (event != NULL) {
        env.payload.body.event = *event;
    } else if (wire_type == ESB_WIRE_EVENT_KEY_STATE) {
        env.payload.body.key_state = local_key_state;
    } else if (wire_type == ESB_WIRE_EVENT_HEARTBEAT) {
        env.payload.body.link_metric.metric = heartbeat_metric;
        env.payload.body.link_metric.value =
            totem_esb_link_metric_value(heartbeat_metric);
        heartbeat_metric = (heartbeat_metric + 1U) % TOTEM_ESB_LINK_METRIC_COUNT;
    }

    size_t evt_env_len = sizeof(env.prefix) + payload_size;
    // LOG_HEXDUMP_DBG(&env, evt_env_len, "ota payload");

    size_t put = ring_buf_put(&tx_buf, (uint8_t *)&env, evt_env_len);
    if (put != evt_env_len) {
        LOG_WRN("Failed to put the whole message (%d vs %d)", put, evt_env_len);
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }

#if ESB_MSG_HAS_POSTFIX
    struct esb_msg_postfix postfix;
    int finalize_err =
        zmk_split_esb_finalize_item((uint8_t *)&env, evt_env_len, false, &postfix);
    if (finalize_err != 0) {
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return finalize_err;
    }

    put = ring_buf_put(&tx_buf, (uint8_t *)&postfix, sizeof(postfix));
    if (put != sizeof(postfix)) {
        LOG_WRN("Failed to put the postfix (%d vs %d)", put, sizeof(postfix));
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }
    // LOG_HEXDUMP_DBG(&postfix, sizeof(postfix), "postfix");
#endif

    static uint16_t evt_msg_id = 0;
    if (++evt_msg_id >= UINT16_MAX - 1000) {
        evt_msg_id = 1;
    }
    // LOG_INF("evt_msg_id: %d", evt_msg_id);

    uint8_t max_retry =
        event != NULL ? get_retry_count(event)
                      : (wire_type == ESB_WIRE_EVENT_KEY_STATE
                             ? CONFIG_ZMK_SPLIT_ESB_RETRY_KEY_POSITION
                             : (wire_type == ESB_WIRE_EVENT_HEARTBEAT ? 1 : 0));
    struct esb_msg_meta meta = {
        .msg_id = evt_msg_id,
        .max_retry = max_retry,
        .pipe = state.tx_pipe,
    };

    put = ring_buf_put(&tx_buf, (uint8_t *)&meta, sizeof(meta));
    if (put != sizeof(meta)) {
        LOG_WRN("Failed to put the meta (%d vs %d)", put, sizeof(meta));
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }
    // LOG_HEXDUMP_DBG(&meta, sizeof(meta), "meta");

    begin_tx();
    k_spin_unlock(&tx_ring_lock, key);

    return 0;
}
#endif

static int report_event_locked(const struct zmk_split_transport_peripheral_event *event) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (get_secure_state() != ESB_V3_ESTABLISHED ||
        k_msgq_num_used_get(&presession_events) > 0) {
        if (k_msgq_put(&presession_events, event, K_NO_WAIT) != 0) {
            totem_esb_transport_queue_pressure(true);
            return -ENOSPC;
        }
        if (get_secure_state() == ESB_V3_ESTABLISHED) {
            k_work_reschedule(&flush_presession_work, K_NO_WAIT);
        }
        return 0;
    }
#endif
    int err = enqueue_wire_event(ESB_WIRE_EVENT_ZMK, event);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    /*
     * A rekey can begin after the lock-free state check above. Preserve the
     * event instead of returning ENOTCONN to ZMK and losing a short tap.
     */
    if (err == -ENOTCONN || err == -EOVERFLOW) {
        if (k_msgq_put(&presession_events, event, K_NO_WAIT) != 0) {
            totem_esb_transport_queue_pressure(true);
            return -ENOSPC;
        }
        if (err == -EOVERFLOW) {
            int rekey_err = begin_fresh_v3_handshake();
            if (rekey_err != 0) {
                k_work_reschedule(&flush_presession_work, K_MSEC(1));
                return 0;
            }
            k_work_reschedule(&handshake_work, K_NO_WAIT);
        }
        return 0;
    }
#endif
    return err;
}

static int
split_peripheral_esb_report_event(const struct zmk_split_transport_peripheral_event *event) {
    k_mutex_lock(&event_mutex, K_FOREVER);
    bool key_event = event->type ==
                     ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT;
    if (key_event) {
        uint32_t position = event->data.key_position_event.position;
        if (position < CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX) {
            uint8_t mask = BIT(position % 8U);
            if (event->data.key_position_event.pressed) {
                local_key_state.keys[position / 8U] |= mask;
            } else {
                local_key_state.keys[position / 8U] &= (uint8_t)~mask;
            }
        }
    }
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (!atomic_get(&transport_ready)) {
        /* Preserve startup edges without touching crypto or waking radio work. */
        int err = k_msgq_put(&presession_events, event, K_NO_WAIT);
        if (err != 0) {
            totem_esb_transport_queue_pressure(true);
            err = -ENOSPC;
        }
        k_mutex_unlock(&event_mutex);
        return err;
    }
    if (key_event) {
        if (get_secure_state() != ESB_V3_ESTABLISHED) {
            /* Resume fast discovery immediately when the user starts typing. */
            totem_esb_backoff_reset(&handshake_backoff,
                                    CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
            k_work_reschedule(&handshake_work, K_NO_WAIT);
        }
    }
#endif
    int err = report_event_locked(event);
    if (key_event && err != 0) {
        /* The local bitmap survives even when the edge queue is full. */
        k_work_reschedule(&key_state_work, K_NO_WAIT);
    }
    k_mutex_unlock(&event_mutex);
    return err;
}

static zmk_split_transport_peripheral_status_changed_cb_t transport_status_cb;
static bool is_enabled = false;

static int split_peripheral_esb_set_enabled(bool enabled) {
    is_enabled = enabled;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    return zmk_split_esb_set_enable(enabled);
#else
    return 0;
#endif
}

static int
split_peripheral_esb_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static struct zmk_split_transport_status split_peripheral_esb_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = is_enabled,
        .connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
    };
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = split_peripheral_esb_report_event,
    .set_enabled = split_peripheral_esb_set_enabled,
    .set_status_callback = split_peripheral_esb_set_status_callback,
    .get_status = split_peripheral_esb_get_status,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &peripheral_api,
                                        CONFIG_ZMK_SPLIT_ESB_PRIORITY);

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
static void handshake_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    enum esb_v3_peripheral_state current = get_secure_state();
    if (current == ESB_V3_NO_SESSION) {
        if (begin_fresh_v3_handshake() != 0) {
            schedule_handshake_retry();
            return;
        }
        current = ESB_V3_WAIT_CHALLENGE;
    }
    if (current == ESB_V3_WAIT_CHALLENGE) {
        (void)enqueue_v3_frame(ESB_WIRE_EVENT_V3_HELLO, NULL);
    } else if (current == ESB_V3_WAIT_SESSION_OK) {
        bool wait_expired;
        k_mutex_lock(&event_mutex, K_FOREVER);
        wait_expired =
            get_secure_state() == ESB_V3_WAIT_SESSION_OK &&
            k_uptime_get() - wait_session_ok_started_at >=
                CONFIG_TOTEM_ESB_V3_SESSION_OK_TIMEOUT_MS;
        k_mutex_unlock(&event_mutex);
        if (wait_expired) {
            /*
             * The central may have discarded this pending key during a long
             * RF outage. A fresh boot nonce is required because continuing
             * to send READY can never recover when the peer has no key.
             */
            if (begin_fresh_v3_handshake() != 0) {
                schedule_handshake_retry();
                return;
            }
            (void)enqueue_v3_frame(ESB_WIRE_EVENT_V3_HELLO, NULL);
        } else {
            (void)enqueue_v3_frame(ESB_WIRE_EVENT_V3_READY, NULL);
        }
    } else {
        return;
    }
    schedule_handshake_retry();
}

static void flush_presession_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&event_mutex, K_FOREVER);
    if (get_secure_state() != ESB_V3_ESTABLISHED) {
        k_mutex_unlock(&event_mutex);
        return;
    }
    struct zmk_split_transport_peripheral_event event;
    uint8_t flushed = 0;
    while (flushed < ESB_V3_PRESESSION_FLUSH_BATCH &&
           k_msgq_peek(&presession_events, &event) == 0) {
        int err = enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event);
        if (err != 0) {
            if (err == -EOVERFLOW &&
                begin_fresh_v3_handshake() == 0) {
                k_work_reschedule(&handshake_work, K_NO_WAIT);
            }
            k_work_reschedule(&flush_presession_work, K_MSEC(1));
            k_mutex_unlock(&event_mutex);
            return;
        }
        /*
         * Keep the head in place until its encrypted frame is committed.
         * Producers therefore see a non-empty queue and cannot overtake it;
         * a full TX ring also cannot move a press behind its release.
         */
        if (k_msgq_get(&presession_events, &event, K_NO_WAIT) != 0) {
            totem_esb_transport_queue_pressure(true);
            k_mutex_unlock(&event_mutex);
            return;
        }
        flushed++;
    }
    if (k_msgq_num_used_get(&presession_events) > 0) {
        k_work_reschedule(&flush_presession_work, K_NO_WAIT);
    } else {
        /* Append current state only after every retained older edge. */
        k_work_reschedule(&key_state_work, K_NO_WAIT);
    }
    k_mutex_unlock(&event_mutex);
}
#endif

static void key_state_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&event_mutex, K_FOREVER);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (get_secure_state() != ESB_V3_ESTABLISHED) {
        k_mutex_unlock(&event_mutex);
        return;
    }
    if (k_msgq_num_used_get(&presession_events) > 0) {
        /* A snapshot must never be followed by an older presession edge. */
        k_work_reschedule(&flush_presession_work, K_NO_WAIT);
        k_mutex_unlock(&event_mutex);
        return;
    }
#endif
    int err = enqueue_wire_event(ESB_WIRE_EVENT_KEY_STATE, NULL);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (err == -EOVERFLOW && begin_fresh_v3_handshake() == 0) {
        k_work_reschedule(&handshake_work, K_NO_WAIT);
    }
#endif
    if (err == -ENOSPC) {
        /* Preserve input capacity while a full radio queue drains. */
        k_work_reschedule(&key_state_work, K_MSEC(10));
    }
    k_mutex_unlock(&event_mutex);
}

static void heartbeat_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_work_cb);

static void heartbeat_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (get_secure_state() == ESB_V3_ESTABLISHED) {
        int err = enqueue_wire_event(ESB_WIRE_EVENT_V3_RECOVERY, NULL);
        if (err == -EOVERFLOW) {
            if (begin_fresh_v3_handshake() == 0) {
                k_work_reschedule(&handshake_work, K_NO_WAIT);
            }
        }
    }
#else
    enqueue_wire_event(ESB_WIRE_EVENT_HEARTBEAT, NULL);
#endif
    k_work_reschedule(&key_state_work, K_NO_WAIT);
    k_work_reschedule(&heartbeat_work, K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
static void benchmark_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(benchmark_work, benchmark_work_cb);

static void benchmark_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (get_secure_state() == ESB_V3_ESTABLISHED) {
        int err = enqueue_wire_event(ESB_WIRE_EVENT_BENCHMARK, NULL);
        if (err == -EOVERFLOW &&
            begin_fresh_v3_handshake() == 0) {
            k_work_reschedule(&handshake_work, K_NO_WAIT);
        }
    }
#else
    enqueue_wire_event(ESB_WIRE_EVENT_BENCHMARK, NULL);
#endif
    k_work_reschedule(&benchmark_work, K_USEC(CONFIG_TOTEM_ESB_BENCHMARK_PERIOD_US));
}
#endif

static void notify_transport_status(void) {
    if (transport_status_cb) {
        transport_status_cb(&esb_peripheral, split_peripheral_esb_get_status());
    }
}

static void notify_status_work_cb(struct k_work *_work) { notify_transport_status(); }

static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

static bool command_payload_size_is_valid(const struct esb_command_envelope *env) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    const size_t wire_header_size = offsetof(struct esb_command_payload, body);
    if (env->prefix.payload_size < wire_header_size) {
        return false;
    }
    if (env->payload.wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE) {
        return env->prefix.payload_size ==
               wire_header_size + sizeof(env->payload.body.challenge);
    }
    if (env->payload.wire_type == ESB_WIRE_COMMAND_V3_SESSION_OK) {
        return env->prefix.payload_size == wire_header_size;
    }
    if (env->payload.wire_type != ESB_WIRE_COMMAND_ZMK) {
        return false;
    }
    const struct zmk_split_transport_central_command *cmd =
        &env->payload.body.cmd;
    const size_t header_size = wire_header_size + sizeof(cmd->type);
#else
    const size_t header_size = sizeof(uint8_t) + sizeof(env->payload.cmd.type);
    const struct zmk_split_transport_central_command *cmd = &env->payload.cmd;
#endif
    if (env->prefix.payload_size < header_size) {
        return false;
    }

    ssize_t data_size;
    switch (cmd->type) {
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS:
        data_size = 0;
        break;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR:
        /*
         * The core command handler treats behavior_dev as a C string. ESB
         * provides CRC integrity but no sender authentication, so reject a
         * forged CRC-valid frame that could otherwise trigger an out-of-bounds
         * string read.
         */
        if (memchr(cmd->data.invoke_behavior.behavior_dev, '\0',
                   sizeof(cmd->data.invoke_behavior.behavior_dev)) == NULL) {
            return false;
        }
        data_size = sizeof(cmd->data.invoke_behavior);
        break;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
        data_size = sizeof(cmd->data.set_physical_layout);
        break;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
        data_size = sizeof(cmd->data.set_hid_indicators);
        break;
    default:
        return false;
    }

    return env->prefix.payload_size == header_size + data_size;
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
static int process_v3_downlink(const struct esb_command_envelope *env) {
    const uint8_t source = peripheral_id - 1U;
    if (env->payload.wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE) {
        uint32_t request = env->payload.body.challenge.request_sequence;
        uint64_t reset_session =
            env->payload.body.challenge.reset_session;
        bool reset_requested = reset_session != 0;
        uint64_t received_central_nonce = env->payload.session_id;
        k_mutex_lock(&event_mutex, K_FOREVER);
        if (env->payload.sequence != 0 || received_central_nonce == 0 ||
            env->payload.body.challenge.peripheral_nonce != peripheral_nonce) {
            k_mutex_unlock(&event_mutex);
            return -EINVAL;
        }
        enum esb_v3_peripheral_state current = get_secure_state();
        if (current == ESB_V3_NO_SESSION) {
            k_mutex_unlock(&event_mutex);
            return -ENOTCONN;
        }

        if (reset_requested) {
            bool reset_session_is_current =
                current == ESB_V3_WAIT_CHALLENGE ||
                totem_esb_v3_active_session(source) == reset_session ||
                totem_esb_v3_pending_session(source) == reset_session;
            if (!reset_session_is_current) {
                k_mutex_unlock(&event_mutex);
                return -ESTALE;
            }
            /*
             * The root-authenticated challenge proves the dongle discarded
             * exactly our current active/pending session after a MIC failure.
             * Preserve the boot nonce and root counter, but destroy both
             * traffic keys before deriving the replacement.
             */
            clear_v3_traffic_session_locked();
            set_secure_state(ESB_V3_WAIT_CHALLENGE);
            current = ESB_V3_WAIT_CHALLENGE;
        } else {
            if (current == ESB_V3_WAIT_CHALLENGE) {
                if (request != 0) {
                    k_mutex_unlock(&event_mutex);
                    return -ESTALE;
                }
            } else if (current == ESB_V3_ESTABLISHED) {
                if (request == accepted_challenge_request) {
                    /*
                     * This transcript already established the active key.
                     * Replaying it must never reset traffic sequences and
                     * reuse CCM nonces; another nonce at the same request is
                     * not a valid replacement either.
                     */
                    int duplicate_err =
                        received_central_nonce == central_nonce ? 0 : -EALREADY;
                    k_mutex_unlock(&event_mutex);
                    return duplicate_err;
                }
                /*
                 * ACK payloads are prepared after the triggering RECOVERY,
                 * so a challenge for N can arrive on the packet carrying
                 * N+1. Accept any still-unconsumed request that we actually
                 * sent, not only the latest probe.
                 */
                if (request < accepted_challenge_request ||
                    request > last_probe_sequence) {
                    k_mutex_unlock(&event_mutex);
                    return -ESTALE;
                }
            } else if (current == ESB_V3_WAIT_SESSION_OK) {
                if (request == accepted_challenge_request) {
                    if (received_central_nonce != central_nonce) {
                        k_mutex_unlock(&event_mutex);
                        return -EALREADY;
                    }
                    k_mutex_unlock(&event_mutex);
                    (void)enqueue_v3_frame(ESB_WIRE_EVENT_V3_READY, NULL);
                    return 0;
                }
                /*
                 * A newer authenticated challenge can supersede an orphaned
                 * pending transcript, but an old or never-sent request
                 * cannot roll it back or jump ahead.
                 */
                if (request < accepted_challenge_request ||
                    request > last_probe_sequence) {
                    k_mutex_unlock(&event_mutex);
                    return -ESTALE;
                }
            } else {
                k_mutex_unlock(&event_mutex);
                return -ESTALE;
            }
        }

        uint64_t session_id;
        int err = totem_esb_v3_prepare_pending(
            source, peripheral_nonce, received_central_nonce, &session_id);
        if (err != 0) {
            k_mutex_unlock(&event_mutex);
            if (reset_requested) {
                k_work_reschedule(&handshake_work, K_NO_WAIT);
            }
            return err;
        }
        central_nonce = received_central_nonce;
        wire_session_id = session_id;
        accepted_challenge_request = request;
        wait_session_ok_started_at = k_uptime_get();
        totem_esb_backoff_reset(&handshake_backoff,
                                CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
        set_secure_state(ESB_V3_WAIT_SESSION_OK);
        k_mutex_unlock(&event_mutex);
        (void)enqueue_v3_frame(ESB_WIRE_EVENT_V3_READY, NULL);
        k_work_reschedule(&handshake_work, K_MSEC(
            CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS));
        return 0;
    }

    if (env->payload.wire_type == ESB_WIRE_COMMAND_V3_SESSION_OK) {
        k_mutex_lock(&event_mutex, K_FOREVER);
        if (env->payload.sequence != 0 ||
            env->payload.session_id != wire_session_id) {
            k_mutex_unlock(&event_mutex);
            return -ESTALE;
        }
        if (get_secure_state() == ESB_V3_ESTABLISHED &&
            totem_esb_v3_active_session(source) == wire_session_id) {
            k_mutex_unlock(&event_mutex);
            return 0;
        }
        if (get_secure_state() != ESB_V3_WAIT_SESSION_OK) {
            k_mutex_unlock(&event_mutex);
            return -ESTALE;
        }
        /*
         * WAIT_SESSION_OK prevents new event seals. Wait for any seal that
         * passed the earlier lock-free state check before replacing the
         * active PSA handle and resetting traffic counters.
         */
        int err = totem_esb_v3_activate_pending(source);
        if (err != 0) {
            k_mutex_unlock(&event_mutex);
            return err;
        }
        wire_sequence = 0;
        downlink_sequence = 0;
        wait_session_ok_started_at = 0;
        set_secure_state(ESB_V3_ESTABLISHED);
        k_mutex_unlock(&event_mutex);
        (void)k_work_cancel_delayable(&handshake_work);
        k_work_reschedule(&flush_presession_work, K_NO_WAIT);
        return 0;
    }

    k_mutex_lock(&event_mutex, K_FOREVER);
    if (env->payload.wire_type != ESB_WIRE_COMMAND_ZMK ||
        get_secure_state() != ESB_V3_ESTABLISHED ||
        env->payload.session_id != wire_session_id ||
        env->payload.sequence == 0) {
        k_mutex_unlock(&event_mutex);
        return -ESTALE;
    }
    if (env->payload.sequence <= downlink_sequence) {
        k_mutex_unlock(&event_mutex);
        totem_esb_benchmark_security_drop(
            source, "replay", env->payload.sequence);
        return -EALREADY;
    }
    downlink_sequence = env->payload.sequence;
    k_mutex_unlock(&event_mutex);
    if (env->payload.body.cmd.type ==
        ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS) {
        return 0;
    }
    zmk_split_transport_peripheral_command_handler(
        &esb_peripheral, env->payload.body.cmd);
    return 0;
}
#endif

static int zmk_split_esb_peripheral_init(void) {
    int ret;
    totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, -EINPROGRESS);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    ret = totem_esb_v3_crypto_init();
    if (ret != 0) {
        LOG_ERR("Refusing to start plaintext fallback after crypto failure (%d)",
                ret);
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, ret);
        return ret;
    }
    totem_esb_diag_stage(TOTEM_DIAG_BOOT_NONCE, -EINPROGRESS);
    ret = begin_fresh_v3_handshake();
    totem_esb_diag_stage(TOTEM_DIAG_BOOT_NONCE, ret);
    if (ret != 0) {
        LOG_ERR("Secure ESB boot nonce generation failed (%d)", ret);
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, ret);
        return ret;
    }
#else
    wire_session_id = sys_rand32_get();
    if (wire_session_id == 0) {
        wire_session_id = 1;
    }
#endif
    for (int i = 0; i < CONFIG_ESB_PIPE_COUNT; i++) {
        ring_buf_init(&rx_bufs[i], RX_RING_BUF_SIZE, rx_bufs_data[i]);
    }
    ret = zmk_split_esb_init(APP_ESB_MODE_PTX, zmk_split_esb_on_ptx_esb_callback);
    if (ret < 0) {
        LOG_ERR("zmk_split_esb_init failed (ret %d)", ret);
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, ret);
        return ret;
    }
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    /* Publish readiness only after every synchronous initialization succeeded. */
    atomic_set(&transport_ready, 1);
    k_work_schedule(&handshake_work, K_NO_WAIT);
#endif
    k_work_schedule(&heartbeat_work, K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    k_work_schedule(&benchmark_work, K_USEC(CONFIG_TOTEM_ESB_BENCHMARK_PERIOD_US));
#endif
    k_work_submit(&notify_status_work);
    totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, 0);
    return 0;
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
K_THREAD_STACK_DEFINE(peripheral_start_stack, 4096);
static struct k_thread peripheral_start_thread;

static void peripheral_start_thread_cb(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
#if IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START)
    const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(console)) {
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, -ENODEV);
        return;
    }

    printk("ESB_DIAG USB_START waiting_for_dtr source=%u\n", peripheral_id);
    uint32_t dtr = 0;
    while (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) != 0 || !dtr) {
        k_msleep(100);
    }
    k_msleep(1000);
    printk("ESB_DIAG USB_START transport_init source=%u stack=4096\n", peripheral_id);
    /* Let the independent USB queues deliver the marker before crypto starts. */
    k_msleep(200);
#elif IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTICS)
#if CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS > 0
    printk("ESB_DIAG DELAY_START delay_ms=%u\n",
           (unsigned int)CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS);
    k_msleep(CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS);
#endif
    printk("ESB_DIAG AUTO_START transport_init source=%u stack=4096\n", peripheral_id);
#endif
    int ret = zmk_split_esb_peripheral_init();
#if IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START)
    printk("ESB_DIAG USB_START transport_return source=%u result=%d\n", peripheral_id, ret);
#elif IS_ENABLED(CONFIG_TOTEM_ESB_DIAGNOSTICS)
    printk("ESB_DIAG AUTO_START transport_return source=%u result=%d\n", peripheral_id, ret);
#else
    ARG_UNUSED(ret);
#endif
}

static int peripheral_start_init(void) {
    /* Keep secure startup off the main init stack and shared workqueues. */
    k_thread_create(&peripheral_start_thread, peripheral_start_stack,
                    K_THREAD_STACK_SIZEOF(peripheral_start_stack),
                    peripheral_start_thread_cb, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    return 0;
}

SYS_INIT(peripheral_start_init, APPLICATION, 99);
#else
SYS_INIT(zmk_split_esb_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif

static void process_rx_work_cb(struct k_work *work) {
    for (int pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        struct ring_buf *rx_buf = &state.rx_bufs[pipe];
        while (ring_buf_size_get(rx_buf) > ESB_MSG_WIRE_MIN_SIZE) {
            struct esb_command_envelope env = {0};
            int item_err = zmk_split_esb_get_item(rx_buf, (uint8_t *)&env,
                                                  sizeof(struct esb_command_envelope),
                                                  true, pipe);
            switch (item_err) {
            case 0:
                if (!command_payload_size_is_valid(&env)) {
                    LOG_WRN("Invalid ESB command payload size/type on pipe %d", pipe);
                    break;
                }
                if (env.payload.source != peripheral_id || pipe != peripheral_id) {
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                    LOG_WRN("Ignoring secure command for source %d on pipe %d (expect %d)",
                            env.payload.source, pipe, peripheral_id);
#else
                    LOG_WRN("Ignoring command type %d for source %d on pipe %d (expect %d)",
                            env.payload.cmd.type, env.payload.source, pipe, peripheral_id);
#endif
                    break;
                }
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                {
                    int secure_err = process_v3_downlink(&env);
                    if (secure_err == 0) {
                        /* Count authenticated, accepted handshake receptions. */
                        if (env.payload.wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE) {
                            totem_esb_diag_event(TOTEM_DIAG_CHALLENGE, 0);
                        } else if (env.payload.wire_type ==
                                   ESB_WIRE_COMMAND_V3_SESSION_OK) {
                            totem_esb_diag_event(TOTEM_DIAG_SESSION_OK, 0);
                        }
                    }
                    if (secure_err != 0 && secure_err != -EALREADY) {
                        totem_esb_benchmark_rx_invalid(pipe, secure_err);
                    }
                }
#else
                if (env.payload.cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS) {
                    // begin_tx(); // NOTE: Shall NOT be called from central due to ESB natural.
                    break;
                }
                zmk_split_transport_peripheral_command_handler(&esb_peripheral, env.payload.cmd);
#endif
                break;
            case -EAGAIN:
                /*
                 * RX entries are complete ESB payloads, so a truncated frame
                 * is malformed rather than a fragment that can finish later.
                 */
                LOG_WRN("Discarding incomplete ESB command on pipe %d", pipe);
                ring_buf_reset(rx_buf);
                goto next_pipe;
            case -EACCES:
                totem_esb_benchmark_rx_invalid(pipe, item_err);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                /*
                 * A root-key CHALLENGE failure is a pre-session probe. Any
                 * pending/active-key MIC failure on this half terminates the
                 * logical encrypted link and starts a fresh handshake.
                 */
                if (env.payload.source == peripheral_id &&
                    pipe == peripheral_id &&
                    env.payload.wire_type !=
                        ESB_WIRE_COMMAND_V3_CHALLENGE) {
                    LOG_ERR("ESB v3 MIC failure terminated local session");
                    restart_v3_after_auth_failure();
                }
#endif
                break;
            default:
                LOG_WRN("Issue fetching an item from the RX buffer: %d", item_err);
                break;
            }
        }
    next_pipe:
        continue;
    }
}

K_WORK_DEFINE(process_rx_work, process_rx_work_cb);

static void process_rx_cb(uint8_t pipe) {
    k_work_submit(&process_rx_work);
}
