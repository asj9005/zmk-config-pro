/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#include <zmk/stdlib.h>
#include <zmk/behavior.h>
#include <zmk/sensors.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/pointing/input_split.h>
#include <zmk/hid_indicators_types.h>
#include <zmk/physical_layouts.h>

#include <totem/esb_benchmark.h>

#include "app_esb.h"
#include "common.h"

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
#define TX_BUFFER_SIZE                                                                        \
    (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_postfix) +                   \
     sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_postfix))
#else
#define TX_BUFFER_SIZE (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_event_envelope))
#endif

BUILD_ASSERT(TX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB central command plus local metadata exceeds the configured payload");
BUILD_ASSERT(RX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB peripheral event exceeds the configured payload");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT + 1 <= CONFIG_ESB_PIPE_COUNT,
             "Pipe 0 is reserved and every peripheral requires its own pipe");

RING_BUF_DECLARE(tx_buf, TX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_CMD_BUFFER_ITEMS);
static struct k_spinlock tx_ring_lock;

#define RX_RING_BUF_SIZE (RX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS)
struct ring_buf rx_bufs[CONFIG_ESB_PIPE_COUNT];
uint8_t rx_bufs_data[CONFIG_ESB_PIPE_COUNT][RX_RING_BUF_SIZE];

static void process_rx_cb(uint8_t pipe);

static struct zmk_split_esb_state state = {
    .tx_pipe = 0,
    .process_rx_callback = process_rx_cb,
    .tx_buf = &tx_buf,
    .rx_bufs = rx_bufs,
};

static void begin_tx(void) {
    zmk_split_esb_tx(&state);
}

static ssize_t get_payload_data_size(const struct zmk_split_transport_central_command *cmd) {
    switch (cmd->type) {
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS:
        return 0;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR:
        return sizeof(cmd->data.invoke_behavior);
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
        return sizeof(cmd->data.set_physical_layout);
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
        return sizeof(cmd->data.set_hid_indicators);
    default:
        return -ENOTSUP;
    }
}

static int split_central_esb_send_command(uint8_t source,
                                          struct zmk_split_transport_central_command cmd) {
    if (source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT) {
        return -EINVAL;
    }
    if (!totem_esb_peer_is_connected(source)) {
        return -ENOTCONN;
    }
    uint8_t wire_source = source + 1;

    ssize_t data_size = get_payload_data_size(&cmd);
    if (data_size < 0) {
        LOG_WRN("Failed to determine payload data size %d", data_size);
        return data_size;
    }

    size_t payload_size = data_size
                        + sizeof(wire_source)
                        + sizeof(enum zmk_split_transport_central_command_type);

    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);

    if (ring_buf_space_get(&tx_buf) < ESB_MSG_EXTRA_SIZE + payload_size) {
        LOG_WRN("No room to send command to the peripheral %d (have %d but only space for %d/%d)",
                source, ESB_MSG_EXTRA_SIZE + payload_size, ring_buf_space_get(&tx_buf),
                ring_buf_capacity_get(&tx_buf));
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }

    struct esb_command_envelope env = {.prefix = {
                                            .magic_prefix = ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                                            .payload_size = payload_size,
                                        },
                                        .payload = {
                                            .source = wire_source,
                                            .cmd = cmd,
                                        }};

    size_t cmd_env_len = sizeof(env.prefix) + payload_size;
    // LOG_HEXDUMP_DBG(&env, cmd_env_len, "Payload");

    size_t put = ring_buf_put(&tx_buf, (uint8_t *)&env, cmd_env_len);
    if (put != cmd_env_len) {
        LOG_WRN("Failed to put the whole message (%d vs %d)", put, cmd_env_len);
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
    struct esb_msg_postfix postfix = {.crc = crc32_ieee((void *)&env, cmd_env_len)};

    put = ring_buf_put(&tx_buf, (uint8_t *)&postfix, sizeof(postfix));
    if (put != sizeof(postfix)) {
        LOG_WRN("Failed to put the postfix (%d vs %d)", put, sizeof(postfix));
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }
#endif

    static uint16_t cmd_msg_id = 0;
    if (++cmd_msg_id >= UINT16_MAX - 1000) {
        cmd_msg_id = 1;
    }
    // LOG_INF("cmd_msg_id: %d", cmd_msg_id);

    uint8_t max_retry = CONFIG_ZMK_SPLIT_ESB_RETRY_CMD;
    struct esb_msg_meta meta = {
        .msg_id = cmd_msg_id,
        .max_retry = max_retry,
        .pipe = wire_source,
    };

    put = ring_buf_put(&tx_buf, (uint8_t *)&meta, sizeof(meta));
    if (put != sizeof(meta)) {
        LOG_WRN("Failed to put the meta (%d vs %d)", put, sizeof(meta));
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }

    begin_tx();
    k_spin_unlock(&tx_ring_lock, key);

    return 0;
}

void zmk_split_esb_on_prx_esb_callback(app_esb_event_t *event) {
    zmk_split_esb_cb(event, &state);
}

static int split_central_esb_get_available_source_ids(uint8_t *sources) {
    int count = 0;
    for (uint8_t source = 0; source < CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT; source++) {
        if (totem_esb_peer_is_connected(source)) {
            sources[count++] = source;
        }
    }
    return count;
}

static zmk_split_transport_central_status_changed_cb_t transport_status_cb;
static bool is_enabled;

static int split_central_esb_set_enabled(bool enabled) {
    is_enabled = enabled;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    return zmk_split_esb_set_enable(enabled);
#else
    return 0;
#endif
}

static int
split_central_esb_set_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static struct zmk_split_transport_status split_central_esb_get_status() {
    uint8_t connected = totem_esb_peer_connected_count();
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = is_enabled,
        .connections = connected == 0
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED
                           : (connected == CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT
                                  ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                  : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED),
    };
}

static const struct zmk_split_transport_central_api central_api = {
    .send_command = split_central_esb_send_command,
    .get_available_source_ids = split_central_esb_get_available_source_ids,
    .set_enabled = split_central_esb_set_enabled,
    .set_status_callback = split_central_esb_set_status_callback,
    .get_status = split_central_esb_get_status,
};

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(esb_central, &central_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static void notify_transport_status(void) {
    if (transport_status_cb) {
        transport_status_cb(&esb_central, split_central_esb_get_status());
    }
}

void totem_esb_notify_transport_status(void) { notify_transport_status(); }

static void notify_status_work_cb(struct k_work *_work) { notify_transport_status(); }

static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

static int zmk_split_esb_central_init(void) {
    for (int i = 0; i < CONFIG_ESB_PIPE_COUNT; i++) {
        ring_buf_init(&rx_bufs[i], RX_RING_BUF_SIZE, rx_bufs_data[i]);
    }
    int ret = zmk_split_esb_init(APP_ESB_MODE_PRX, zmk_split_esb_on_prx_esb_callback);
    if (ret) {
        LOG_ERR("zmk_split_esb_init failed (err %d)", ret);
        return ret;
    }
    k_work_submit(&notify_status_work);
    return 0;
}

SYS_INIT(zmk_split_esb_central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

extern const struct zmk_split_transport_central *active_transport;

struct esb_rx_sequence_state {
    bool session_initialized;
    bool initialized;
    uint32_t session_id;
    uint32_t last;
    uint32_t received;
    uint32_t gaps;
    uint32_t duplicates;
    uint32_t out_of_order;
    uint32_t session_changes;
};

static struct esb_rx_sequence_state
    rx_sequences[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT];
static uint8_t
    key_pos_states[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT]
                  [(CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX + 7) / 8];

static bool event_payload_size_is_valid(const struct esb_event_envelope *env) {
    const size_t header_size = sizeof(uint8_t) * 2 + sizeof(uint32_t) * 3;
    if (env->prefix.payload_size < header_size) {
        return false;
    }

    if (env->payload.wire_type == ESB_WIRE_EVENT_HEARTBEAT) {
        return env->prefix.payload_size ==
                   header_size + sizeof(env->payload.body.link_metric) &&
               env->payload.body.link_metric.metric < TOTEM_ESB_LINK_METRIC_COUNT;
    }
    if (env->payload.wire_type == ESB_WIRE_EVENT_BENCHMARK) {
        return env->prefix.payload_size == header_size;
    }
    if (env->payload.wire_type != ESB_WIRE_EVENT_ZMK ||
        env->prefix.payload_size < header_size + sizeof(env->payload.body.event.type)) {
        return false;
    }

    ssize_t data_size;
    switch (env->payload.body.event.type) {
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT:
        data_size = sizeof(env->payload.body.event.data.input_event);
        break;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT:
        data_size = sizeof(env->payload.body.event.data.key_position_event);
        break;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT:
        data_size = sizeof(env->payload.body.event.data.sensor_event);
        break;
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT:
        data_size = sizeof(env->payload.body.event.data.battery_event);
        break;
    default:
        return false;
    }

    return env->prefix.payload_size ==
           header_size + sizeof(env->payload.body.event.type) + data_size;
}

static void release_source_keys(uint8_t source) {
    uint8_t *source_keys = key_pos_states[source];
    for (uint8_t position = 0; position < CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX;
         position++) {
        if ((source_keys[position / 8] >> (position % 8)) & 1U) {
            raise_zmk_position_state_changed((struct zmk_position_state_changed){
                .source = source,
                .position = position,
                .state = false,
                .timestamp = k_uptime_get(),
            });
        }
    }
    memset(source_keys, 0, sizeof(key_pos_states[source]));
}

void totem_esb_source_disconnected(uint8_t source) {
    if (source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT) {
        return;
    }
    release_source_keys(source);
    rx_sequences[source].initialized = false;
}

static void update_source_session(uint8_t source, uint32_t session_id) {
    struct esb_rx_sequence_state *seq = &rx_sequences[source];
    if (seq->session_initialized && seq->session_id == session_id) {
        return;
    }

    if (seq->session_initialized) {
        release_source_keys(source);
        seq->session_changes++;
        LOG_INF("ESB source %u started session %u (previous %u)", source, session_id,
                seq->session_id);
    }
    seq->session_initialized = true;
    seq->initialized = false;
    seq->session_id = session_id;
}

static void process_rx_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    for (int pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        struct ring_buf *rx_buf = &state.rx_bufs[pipe];
        while (ring_buf_size_get(rx_buf) > ESB_MSG_WIRE_MIN_SIZE) {
            struct esb_event_envelope env = {0};
            int item_err = zmk_split_esb_get_item(rx_buf, (uint8_t *)&env,
                                                  sizeof(struct esb_event_envelope));
            switch (item_err) {
            case 0: {
                if (!event_payload_size_is_valid(&env)) {
                    LOG_WRN("Invalid ESB event payload size/type on pipe %d", pipe);
                    totem_esb_benchmark_rx_invalid(pipe, -EMSGSIZE);
                    break;
                }
                if (env.payload.source == 0 ||
                    env.payload.source > CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT) {
                    LOG_WRN("Invalid ESB wire source %u", env.payload.source);
                    break;
                }
                if (pipe != env.payload.source) {
                    LOG_WRN("ESB source %u arrived on unexpected pipe %d",
                            env.payload.source, pipe);
                    totem_esb_benchmark_rx_invalid(pipe, -EADDRNOTAVAIL);
                    break;
                }

                uint8_t source = env.payload.source - 1;
                update_source_session(source, env.payload.session_id);
                struct esb_rx_sequence_state *seq = &rx_sequences[source];
                uint32_t gap = 0;
                bool accept = true;

                if (seq->initialized) {
                    uint32_t delta = env.payload.sequence - seq->last;
                    if (delta == 0) {
                        seq->duplicates++;
                        accept = false;
                    } else if (delta < (UINT32_MAX / 2U) + 1U) {
                        gap = delta - 1U;
                        seq->gaps += gap;
                        seq->last = env.payload.sequence;
                    } else {
                        seq->out_of_order++;
                        accept = false;
                    }
                } else {
                    seq->initialized = true;
                    seq->last = env.payload.sequence;
                }
                seq->received++;

                totem_esb_peer_seen(source);

                uint8_t event_type = 0;
                uint8_t position = 0;
                uint8_t pressed = 0;
                if (env.payload.wire_type == ESB_WIRE_EVENT_ZMK) {
                    event_type = env.payload.body.event.type;
                    if (env.payload.body.event.type ==
                        ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
                        position = env.payload.body.event.data.key_position_event.position;
                        pressed = env.payload.body.event.data.key_position_event.pressed;
                    }
                } else if (env.payload.wire_type == ESB_WIRE_EVENT_HEARTBEAT) {
                    totem_esb_benchmark_link_metric(
                        source, env.payload.session_id,
                        env.payload.body.link_metric.metric,
                        env.payload.body.link_metric.value);
                }
                bool accepted_for_zmk =
                    accept && env.payload.wire_type == ESB_WIRE_EVENT_ZMK &&
                    &esb_central == active_transport;
                totem_esb_benchmark_rx(
                    source, env.payload.session_id, env.payload.sequence, gap,
                    env.payload.source_tick, env.payload.wire_type, event_type, position,
                    pressed, accepted_for_zmk);

                if (!accepted_for_zmk) {
                    break;
                }

                struct zmk_split_transport_peripheral_event ev = env.payload.body.event;
                if (ev.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT &&
                    position < CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX) {
                    uint8_t *source_keys = key_pos_states[source];
                    if (pressed) {
                        if ((source_keys[position / 8] >> (position % 8)) & 1U) {
                            LOG_WRN("Repeated press on source %u position %u; injecting release",
                                    source, position);
                            raise_zmk_position_state_changed((struct zmk_position_state_changed){
                                .source = source,
                                .position = position,
                                .state = false,
                                .timestamp = k_uptime_get(),
                            });
                        }
                        source_keys[position / 8] |= 1U << (position % 8);
                    } else {
                        source_keys[position / 8] &= ~(1U << (position % 8));
                    }
                }

                zmk_split_transport_central_peripheral_event_handler(
                    &esb_central, source, env.payload.body.event);
                if (ev.type ==
                    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT) {
                    /*
                     * Prospector uses one deferred state slot for peripheral
                     * UI events. Replay both authoritative sources, spaced by
                     * the display-sync worker, so simultaneous battery reports
                     * cannot leave one circle stale.
                     */
                    totem_esb_schedule_display_sync();
                }
                break;
            }
            case -EAGAIN:
                /*
                 * Each ESB RX callback inserts one complete radio payload.
                 * An incomplete frame therefore cannot become complete later;
                 * discard it instead of spinning the system workqueue.
                 */
                totem_esb_benchmark_rx_invalid(pipe, item_err);
                ring_buf_reset(rx_buf);
                goto next_pipe;
            case -EPROTO:
            case -EMSGSIZE:
            case -EBADMSG:
            case -EINVAL:
                totem_esb_benchmark_rx_invalid(pipe, item_err);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP)
                if (item_err == -EBADMSG) {
                    esb_rf_ch_hop();
                }
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP) */
                break;
            default:
                totem_esb_benchmark_rx_invalid(pipe, item_err);
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
