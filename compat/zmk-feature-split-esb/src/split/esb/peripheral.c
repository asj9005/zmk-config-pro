/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

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

#include "app_esb.h"
#include "common.h"

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
#define TX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_postfix) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_postfix))
#else
#define TX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_command_envelope))
#endif

BUILD_ASSERT(TX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB peripheral event plus local metadata exceeds the configured payload");
BUILD_ASSERT(RX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB central command exceeds the configured payload");

RING_BUF_DECLARE(tx_buf, TX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS);
static struct k_spinlock tx_ring_lock;

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

void zmk_split_esb_on_ptx_esb_callback(app_esb_event_t *event) {
    zmk_split_esb_cb(event, &state);
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC)
    struct esb_msg_postfix postfix = {.crc = crc32_ieee((void *)&env, evt_env_len)};

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
                      : (wire_type == ESB_WIRE_EVENT_HEARTBEAT ? 1 : 0);
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

static int
split_peripheral_esb_report_event(const struct zmk_split_transport_peripheral_event *event) {
    return enqueue_wire_event(ESB_WIRE_EVENT_ZMK, event);
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

static void heartbeat_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_work_cb);

static void heartbeat_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    enqueue_wire_event(ESB_WIRE_EVENT_HEARTBEAT, NULL);
    k_work_reschedule(&heartbeat_work, K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
static void benchmark_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(benchmark_work, benchmark_work_cb);

static void benchmark_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    enqueue_wire_event(ESB_WIRE_EVENT_BENCHMARK, NULL);
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
    const size_t header_size = sizeof(uint8_t) + sizeof(env->payload.cmd.type);
    if (env->prefix.payload_size < header_size) {
        return false;
    }

    ssize_t data_size;
    switch (env->payload.cmd.type) {
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
        if (memchr(env->payload.cmd.data.invoke_behavior.behavior_dev, '\0',
                   sizeof(env->payload.cmd.data.invoke_behavior.behavior_dev)) == NULL) {
            return false;
        }
        data_size = sizeof(env->payload.cmd.data.invoke_behavior);
        break;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
        data_size = sizeof(env->payload.cmd.data.set_physical_layout);
        break;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
        data_size = sizeof(env->payload.cmd.data.set_hid_indicators);
        break;
    default:
        return false;
    }

    return env->prefix.payload_size == header_size + data_size;
}

static int zmk_split_esb_peripheral_init(void) {
    wire_session_id = sys_rand32_get();
    if (wire_session_id == 0) {
        wire_session_id = 1;
    }
    for (int i = 0; i < CONFIG_ESB_PIPE_COUNT; i++) {
        ring_buf_init(&rx_bufs[i], RX_RING_BUF_SIZE, rx_bufs_data[i]);
    }
    int ret = zmk_split_esb_init(APP_ESB_MODE_PTX, zmk_split_esb_on_ptx_esb_callback);
    if (ret < 0) {
        LOG_ERR("zmk_split_esb_init failed (ret %d)", ret);
        return ret;
    }
    k_work_schedule(&heartbeat_work, K_MSEC(CONFIG_TOTEM_ESB_HEARTBEAT_INTERVAL_MS));
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    k_work_schedule(&benchmark_work, K_USEC(CONFIG_TOTEM_ESB_BENCHMARK_PERIOD_US));
#endif
    k_work_submit(&notify_status_work);
    return 0;
}

SYS_INIT(zmk_split_esb_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static void process_rx_work_cb(struct k_work *work) {
    for (int pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        struct ring_buf *rx_buf = &state.rx_bufs[pipe];
        while (ring_buf_size_get(rx_buf) > ESB_MSG_WIRE_MIN_SIZE) {
            struct esb_command_envelope env = {0};
            int item_err = zmk_split_esb_get_item(rx_buf, (uint8_t *)&env,
                                                  sizeof(struct esb_command_envelope));
            switch (item_err) {
            case 0:
                if (!command_payload_size_is_valid(&env)) {
                    LOG_WRN("Invalid ESB command payload size/type on pipe %d", pipe);
                    break;
                }
                if (env.payload.source != peripheral_id || pipe != peripheral_id) {
                    LOG_WRN("Ignoring command type %d for source %d on pipe %d (expect %d)",
                            env.payload.cmd.type, env.payload.source, pipe, peripheral_id);
                    break;
                }
                if (env.payload.cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS) {
                    // begin_tx(); // NOTE: Shall NOT be called from central due to ESB natural.
                    break;
                }
                zmk_split_transport_peripheral_command_handler(&esb_peripheral, env.payload.cmd);
                break;
            case -EAGAIN:
                /*
                 * RX entries are complete ESB payloads, so a truncated frame
                 * is malformed rather than a fragment that can finish later.
                 */
                LOG_WRN("Discarding incomplete ESB command on pipe %d", pipe);
                ring_buf_reset(rx_buf);
                goto next_pipe;
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
