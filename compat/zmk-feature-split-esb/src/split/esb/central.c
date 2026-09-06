/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <stddef.h>

#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
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
#include <totem/esb_diagnostics.h>
#include <totem/esb_key_state.h>
#include <totem/owned_swapper.h>
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
#include <totem/esb_v3_crypto.h>
#endif

#include "app_esb.h"
#include "common.h"

#if ESB_MSG_HAS_POSTFIX
#define TX_BUFFER_SIZE                                                                        \
    (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_postfix) +                   \
     sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_event_envelope) + sizeof(struct esb_msg_postfix))
#else
#define TX_BUFFER_SIZE (sizeof(struct esb_command_envelope) + sizeof(struct esb_msg_meta))
#define RX_BUFFER_SIZE (sizeof(struct esb_event_envelope))
#endif

BUILD_ASSERT(TX_BUFFER_SIZE - sizeof(struct esb_msg_meta) <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB central command plus local metadata exceeds the configured payload");
BUILD_ASSERT(RX_BUFFER_SIZE <= CONFIG_ESB_MAX_PAYLOAD_LENGTH,
             "ESB peripheral event exceeds the configured payload");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT + 1 <= CONFIG_ESB_PIPE_COUNT,
             "Pipe 0 is reserved and every peripheral requires its own pipe");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX > 0 &&
                 CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX <= 256,
             "Key snapshots require 1 to 256 positions");

RING_BUF_DECLARE(tx_buf, TX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_CMD_BUFFER_ITEMS);
static struct k_spinlock tx_ring_lock;

#define RX_RING_BUF_SIZE (RX_BUFFER_SIZE * CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS)
struct ring_buf rx_bufs[CONFIG_ESB_PIPE_COUNT];
/* Pipe 0 is reserved. Keep each real peer's capacity, without backing storage
 * for a pipe whose frames would always fail the source check. */
uint8_t rx_bufs_data[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT][RX_RING_BUF_SIZE];

static void init_rx_buffers(void) {
    for (uint8_t pipe = 1; pipe <= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT; pipe++) {
        ring_buf_init(&rx_bufs[pipe], RX_RING_BUF_SIZE, rx_bufs_data[pipe - 1U]);
    }
}

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

/* Drop a retired peer's commands at every queue level. Other pipes keep their
 * relative order. The radio driver defers hardware cleanup if an ACK is active.
 */
static void drop_source_commands(uint8_t source) {
    uint8_t pipe = source + 1U;
    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);
    size_t remaining = ring_buf_size_get(&tx_buf);
    while (remaining > 0) {
        struct esb_msg_prefix prefix;
        if (ring_buf_peek(&tx_buf, (uint8_t *)&prefix, sizeof(prefix)) != sizeof(prefix)) {
            ring_buf_reset(&tx_buf);
            break;
        }
        size_t frame_size = ESB_MSG_EXTRA_SIZE + prefix.payload_size;
        uint8_t frame[TX_BUFFER_SIZE];
        if (memcmp(prefix.magic_prefix, ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                   sizeof(prefix.magic_prefix)) != 0 ||
            frame_size > sizeof(frame) || frame_size > remaining ||
            ring_buf_get(&tx_buf, frame, frame_size) != frame_size) {
            ring_buf_reset(&tx_buf);
            break;
        }
        remaining -= frame_size;
        struct esb_msg_meta meta;
        memcpy(&meta, &frame[frame_size - sizeof(meta)], sizeof(meta));
        if (meta.pipe != pipe) {
            ring_buf_put(&tx_buf, frame, frame_size);
        }
    }
    zmk_split_esb_flush_pipe(pipe);
    k_spin_unlock(&tx_ring_lock, key);
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

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
struct esb_v3_central_peer {
    uint64_t pending_peripheral_nonce;
    uint64_t active_peripheral_nonce;
    uint64_t central_nonce;
    uint64_t active_central_nonce;
    uint64_t pending_session;
    uint64_t active_session;
    uint64_t challenge_reset_session;
    uint64_t recovery_peripheral_nonce;
    uint32_t request_sequence;
    uint32_t last_recovery_sequence;
    uint32_t down_sequence;
    int64_t challenge_queued_at;
    int64_t pending_started_at;
    bool pending;
    atomic_t active;
};

static struct esb_v3_central_peer
    secure_peers[CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT];
K_MUTEX_DEFINE(command_mutex);

static int enqueue_v3_downlink(
    uint8_t source, enum esb_wire_command_type wire_type,
    const struct zmk_split_transport_central_command *cmd,
    uint64_t session_id, uint32_t sequence, uint64_t peripheral_nonce,
    uint32_t request_sequence, uint64_t reset_session) {
    uint8_t wire_source = source + 1U;
    size_t body_size = 0;
    if (wire_type == ESB_WIRE_COMMAND_ZMK) {
        if (cmd == NULL) {
            return -EINVAL;
        }
        ssize_t data_size = get_payload_data_size(cmd);
        if (data_size < 0) {
            return data_size;
        }
        body_size = sizeof(cmd->type) + data_size;
    } else if (wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE) {
        body_size = sizeof(((struct esb_command_payload *)0)->body.challenge);
    } else if (wire_type != ESB_WIRE_COMMAND_V3_SESSION_OK) {
        return -ENOTSUP;
    }

    size_t header_size = offsetof(struct esb_command_payload, body);
    size_t payload_size = header_size + body_size;
    struct esb_command_envelope env = {
        .prefix =
            {
                .magic_prefix = ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX,
                .payload_size = payload_size,
            },
        .payload =
            {
                .source = wire_source,
                .wire_type = wire_type,
                .sequence = sequence,
                .session_id = session_id,
                .source_tick = wire_type == ESB_WIRE_COMMAND_ZMK
                                   ? k_cycle_get_32()
                                   : 0,
            },
    };
    if (wire_type == ESB_WIRE_COMMAND_ZMK) {
        env.payload.body.cmd = *cmd;
    } else if (wire_type == ESB_WIRE_COMMAND_V3_CHALLENGE) {
        env.payload.body.challenge.peripheral_nonce = peripheral_nonce;
        env.payload.body.challenge.request_sequence = request_sequence;
        env.payload.body.challenge.reset_session = reset_session;
    }

    size_t env_len = sizeof(env.prefix) + payload_size;
    struct esb_msg_postfix postfix;
    int err =
        zmk_split_esb_finalize_item((uint8_t *)&env, env_len, true, &postfix);
    if (err != 0) {
        return err;
    }

    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);
    size_t frame_size = env_len + sizeof(postfix) + sizeof(struct esb_msg_meta);
    if (ring_buf_space_get(&tx_buf) < frame_size) {
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }
    size_t put = ring_buf_put(&tx_buf, (uint8_t *)&env, env_len);
    put += ring_buf_put(&tx_buf, (uint8_t *)&postfix, sizeof(postfix));
    static uint16_t command_message_id;
    if (++command_message_id >= UINT16_MAX - 1000U) {
        command_message_id = 1;
    }
    struct esb_msg_meta meta = {
        .msg_id = command_message_id,
        .max_retry = CONFIG_ZMK_SPLIT_ESB_RETRY_CMD,
        .pipe = wire_source,
    };
    put += ring_buf_put(&tx_buf, (uint8_t *)&meta, sizeof(meta));
    if (put != frame_size) {
        LOG_ERR("Unable to queue complete secure ESB downlink");
        ring_buf_reset(&tx_buf);
        k_spin_unlock(&tx_ring_lock, key);
        return -ENOSPC;
    }
    begin_tx();
    k_spin_unlock(&tx_ring_lock, key);
    return 0;
}

static int split_central_esb_send_command(
    uint8_t source, struct zmk_split_transport_central_command cmd) {
    k_mutex_lock(&command_mutex, K_FOREVER);
    if (source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT ||
        !atomic_get(&secure_peers[source].active) ||
        !totem_esb_peer_is_connected(source)) {
        int err = source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT
                      ? -EINVAL
                      : -ENOTCONN;
        k_mutex_unlock(&command_mutex);
        return err;
    }
    if (secure_peers[source].down_sequence >= UINT32_MAX - 1U) {
        k_mutex_unlock(&command_mutex);
        return -EOVERFLOW;
    }
    uint32_t sequence = secure_peers[source].down_sequence + 1U;
    int err = enqueue_v3_downlink(
        source, ESB_WIRE_COMMAND_ZMK, &cmd,
        secure_peers[source].active_session, sequence, 0, 0, 0);
    if (err == 0) {
        secure_peers[source].down_sequence = sequence;
    }
    k_mutex_unlock(&command_mutex);
    return err;
}
#else
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

#if ESB_MSG_HAS_POSTFIX
    struct esb_msg_postfix postfix;
    int finalize_err =
        zmk_split_esb_finalize_item((uint8_t *)&env, cmd_env_len, true, &postfix);
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
#endif

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
    totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, -EINPROGRESS);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    int crypto_err = totem_esb_v3_crypto_init();
    if (crypto_err != 0) {
        LOG_ERR("Refusing to start plaintext fallback after crypto failure (%d)",
                crypto_err);
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, crypto_err);
        return crypto_err;
    }
#endif
    init_rx_buffers();
    int ret = zmk_split_esb_init(APP_ESB_MODE_PRX, zmk_split_esb_on_prx_esb_callback);
    if (ret) {
        LOG_ERR("zmk_split_esb_init failed (err %d)", ret);
        totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, ret);
        return ret;
    }
    k_work_submit(&notify_status_work);
    totem_esb_diag_stage(TOTEM_DIAG_TRANSPORT, 0);
    return 0;
}

SYS_INIT(zmk_split_esb_central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

extern const struct zmk_split_transport_central *active_transport;

struct esb_rx_sequence_state {
    bool session_initialized;
    bool initialized;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    uint64_t session_id;
#else
    uint32_t session_id;
#endif
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
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    const size_t header_size = offsetof(struct esb_event_payload, body);
#else
    const size_t header_size = sizeof(uint8_t) * 2 + sizeof(uint32_t) * 3;
#endif
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
    if (env->payload.wire_type == ESB_WIRE_EVENT_KEY_STATE) {
        return env->prefix.payload_size == header_size + sizeof(env->payload.body.key_state) &&
               totem_esb_key_state_valid(env->payload.body.key_state.keys,
                                         CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX);
    }
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    if (env->payload.wire_type == ESB_WIRE_EVENT_V3_HELLO ||
        env->payload.wire_type == ESB_WIRE_EVENT_V3_READY) {
        return env->prefix.payload_size == header_size;
    }
    if (env->payload.wire_type == ESB_WIRE_EVENT_V3_RECOVERY) {
        return env->prefix.payload_size ==
                   header_size + sizeof(env->payload.body.recovery) &&
               env->payload.body.recovery.link_metric.metric <
                   TOTEM_ESB_LINK_METRIC_COUNT;
    }
#endif
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
    for (uint16_t position = 0; position < CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX;
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
    /* A hold-tap release above can start a latched behavior. Clear
     * this source's swapper after all synthetic releases complete. */
    totem_owned_swapper_source_reset(source);
}

static void emit_snapshot_key(void *context, uint8_t position, bool pressed) {
    uint8_t source = *(uint8_t *)context;
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };
    zmk_split_transport_central_peripheral_event_handler(&esb_central, source, event);
}

void totem_esb_source_disconnected(uint8_t source) {
    if (source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT) {
        return;
    }
    release_source_keys(source);
    rx_sequences[source].initialized = false;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    k_mutex_lock(&command_mutex, K_FOREVER);
    atomic_set(&secure_peers[source].active, false);
    secure_peers[source].pending = false;
    secure_peers[source].active_session = 0;
    secure_peers[source].pending_session = 0;
    secure_peers[source].challenge_reset_session = 0;
    secure_peers[source].pending_peripheral_nonce = 0;
    secure_peers[source].central_nonce = 0;
    secure_peers[source].request_sequence = 0;
    secure_peers[source].challenge_queued_at = 0;
    secure_peers[source].pending_started_at = 0;
    secure_peers[source].active_peripheral_nonce = 0;
    secure_peers[source].active_central_nonce = 0;
    drop_source_commands(source);
    totem_esb_v3_discard_pending(source);
    totem_esb_v3_clear_active(source);
    k_mutex_unlock(&command_mutex);
#else
    drop_source_commands(source);
#endif
}

static void update_source_session(
    uint8_t source,
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
    uint64_t session_id,
#else
    uint32_t session_id,
#endif
    bool force) {
    struct esb_rx_sequence_state *seq = &rx_sequences[source];
    if (!force && seq->session_initialized && seq->session_id == session_id) {
        return;
    }

    if (seq->session_initialized) {
#if !IS_ENABLED(CONFIG_TOTEM_ESB_V3)
        drop_source_commands(source);
#endif
        release_source_keys(source);
        seq->session_changes++;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
        LOG_INF("ESB source %u started session %llu (previous %llu)", source,
                (unsigned long long)session_id,
                (unsigned long long)seq->session_id);
#else
        LOG_INF("ESB source %u started session %u (previous %u)", source,
                session_id, seq->session_id);
#endif
    }
    seq->session_initialized = true;
    seq->initialized = false;
    seq->session_id = session_id;
}

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
static int secure_random_u64(uint64_t *value) {
    int err = sys_csrand_get(value, sizeof(*value));
    if (err != 0) {
        return err;
    }
    return *value == 0 ? -EIO : 0;
}

static int queue_v3_challenge(uint8_t source) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    int err = enqueue_v3_downlink(
        source, ESB_WIRE_COMMAND_V3_CHALLENGE, NULL, peer->central_nonce, 0,
        peer->pending_peripheral_nonce, peer->request_sequence,
        peer->challenge_reset_session);
    if (err == 0) {
        peer->challenge_queued_at = k_uptime_get();
    }
    return err;
}

static int begin_v3_pending_session(uint8_t source, uint64_t peripheral_nonce,
                                    uint32_t request_sequence,
                                    uint64_t reset_session) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    if (peripheral_nonce == 0) {
        return -EINVAL;
    }
    if (peer->pending &&
        peer->pending_peripheral_nonce == peripheral_nonce &&
        peer->request_sequence == request_sequence &&
        peer->challenge_reset_session == reset_session) {
        if (k_uptime_get() - peer->challenge_queued_at >= 10) {
            return queue_v3_challenge(source);
        }
        return 0;
    }

    uint64_t central_nonce;
    uint64_t session_id;
    int err;
    do {
        err = secure_random_u64(&central_nonce);
        if (err != 0) {
            return err;
        }
        err = totem_esb_v3_prepare_pending(source, peripheral_nonce,
                                           central_nonce, &session_id);
    } while (err == -EAGAIN);
    if (err != 0) {
        return err;
    }

    peer->pending = true;
    peer->pending_peripheral_nonce = peripheral_nonce;
    peer->central_nonce = central_nonce;
    peer->pending_session = session_id;
    peer->request_sequence = request_sequence;
    peer->challenge_reset_session = reset_session;
    peer->challenge_queued_at = 0;
    peer->pending_started_at = k_uptime_get();
    return queue_v3_challenge(source);
}

static int begin_or_repeat_v3_recovery_session(
    uint8_t source, uint64_t peripheral_nonce, uint32_t request_sequence) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    if (peer->pending) {
        if (peer->challenge_reset_session == 0 &&
            peer->pending_peripheral_nonce == peripheral_nonce) {
            /*
             * An ACK payload already queued for this pending transcript can
             * reach the half before this newer RECOVERY is processed. Keep
             * the key, request and root CCM nonce fixed until READY instead
             * of stranding the two sides on different pending sessions.
             */
            return queue_v3_challenge(source);
        }
        /*
         * A HELLO for another boot nonce or an auth-failure reset already
         * owns the pending key slot. Neither may be displaced by a delayed
         * heartbeat from the active traffic session.
         */
        return 0;
    }
    return begin_v3_pending_session(
        source, peripheral_nonce, request_sequence, 0);
}

static void expire_v3_pending_if_needed(uint8_t source) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    if (!peer->pending ||
        k_uptime_get() - peer->pending_started_at <
            CONFIG_TOTEM_ESB_V3_PENDING_TIMEOUT_MS) {
        return;
    }

    bool reset_pending = peer->challenge_reset_session != 0;
    LOG_WRN("ESB v3 source %u pending handshake expired", source);
    peer->pending = false;
    peer->pending_peripheral_nonce = 0;
    peer->central_nonce = 0;
    peer->pending_session = 0;
    peer->request_sequence = 0;
    peer->challenge_reset_session = 0;
    peer->challenge_queued_at = 0;
    peer->pending_started_at = 0;
    totem_esb_v3_discard_pending(source);

    /*
     * On a central cold boot, a single captured normal HELLO/RECOVERY can
     * otherwise pin the wrong boot nonce forever. Let the packet that
     * observes expiry nominate a new root epoch. A MIC-reset transcript
     * already belongs to the live root epoch, so preserve its high-water.
     */
    if (!atomic_get(&peer->active) && !reset_pending) {
        peer->recovery_peripheral_nonce = 0;
        peer->last_recovery_sequence = 0;
    }
}

static void restart_v3_after_auth_failure(
    uint8_t source, const struct esb_event_envelope *env) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    uint64_t peripheral_nonce = 0;
    uint64_t reset_session = 0;
    uint32_t request_sequence = 0;

    /*
     * common.c returns EACCES only after the clear session ID selected an
     * existing key and CCM rejected its tag. Capture that exact key
     * stage/session before teardown so a root-authenticated CHALLENGE can
     * tell the half to abandon the same failed session immediately.
     */
    if (peer->pending &&
        env->payload.wire_type == ESB_WIRE_EVENT_V3_READY &&
        peer->pending_session == env->payload.session_id) {
        peripheral_nonce = peer->pending_peripheral_nonce;
        reset_session = peer->pending_session;
        request_sequence = peer->request_sequence;
    } else if (atomic_get(&peer->active) &&
               peer->active_session == env->payload.session_id) {
        peripheral_nonce = peer->active_peripheral_nonce;
        reset_session = peer->active_session;
        request_sequence = peer->last_recovery_sequence;
    } else if (peer->pending &&
               peer->pending_session == env->payload.session_id) {
        peripheral_nonce = peer->pending_peripheral_nonce;
        reset_session = peer->pending_session;
        request_sequence = peer->request_sequence;
    }

    totem_esb_peer_auth_failed(source);

    if (peripheral_nonce == 0 || reset_session == 0) {
        LOG_ERR("ESB v3 source %u MIC failure had no recoverable transcript",
                source);
        return;
    }

    int err = begin_v3_pending_session(
        source, peripheral_nonce, request_sequence, reset_session);
    if (err != 0) {
        /*
         * The half's next root RECOVERY heartbeat remains the bounded
         * fallback if the immediate challenge cannot be queued.
         */
        LOG_ERR("ESB v3 source %u immediate auth-failure rekey failed (%d)",
                source, err);
    }
}

static int accept_v3_recovery_sequence(
    uint8_t source, struct esb_v3_central_peer *peer,
    uint64_t peripheral_nonce, uint32_t sequence) {
    /*
     * Root RECOVERY nonces use the half's boot nonce, not the traffic
     * session. Keep one high-water mark for the entire boot so a delayed
     * packet from a retired traffic session cannot become fresh again after
     * rekey. A central that has just booted may learn this nonce from the
     * first authenticated RECOVERY; a half with a genuinely new boot nonce
     * starts with HELLO and installs that nonce when READY is promoted.
     */
    if (peer->recovery_peripheral_nonce == 0) {
        if (atomic_get(&peer->active) || peer->pending) {
            return -ESTALE;
        }
        peer->recovery_peripheral_nonce = peripheral_nonce;
        peer->last_recovery_sequence = 0;
    }
    if (peer->recovery_peripheral_nonce != peripheral_nonce) {
        return -ESTALE;
    }
    /*
     * A sender must rekey before wrap, so wrap is never a valid transition.
     * Strict monotonicity makes nonce reuse fail closed if a future sender
     * regression accidentally emits sequence 1 under the same root key.
     */
    if (sequence == 0 || sequence <= peer->last_recovery_sequence) {
        totem_esb_benchmark_security_drop(source, "replay", sequence);
        return -EALREADY;
    }
    peer->last_recovery_sequence = sequence;
    return 0;
}

static void confirm_v3_active_session(uint8_t source) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    if (!peer->pending ||
        peer->pending_peripheral_nonce != peer->active_peripheral_nonce ||
        peer->central_nonce != peer->active_central_nonce) {
        return;
    }
    peer->pending = false;
    peer->pending_peripheral_nonce = 0;
    peer->central_nonce = 0;
    peer->pending_session = 0;
    peer->request_sequence = 0;
    peer->challenge_reset_session = 0;
    peer->challenge_queued_at = 0;
    peer->pending_started_at = 0;
    totem_esb_v3_discard_pending(source);
}

static int process_v3_control_event(uint8_t source,
                                    const struct esb_event_envelope *env) {
    struct esb_v3_central_peer *peer = &secure_peers[source];
    expire_v3_pending_if_needed(source);
    switch (env->payload.wire_type) {
    case ESB_WIRE_EVENT_V3_HELLO:
        if (env->payload.sequence != 0 || env->payload.session_id == 0) {
            return -EINVAL;
        }
        /*
         * A captured HELLO from the current boot must not force a needless
         * rekey. A genuinely rebooted/rekeying half presents a new nonce.
         */
        if (atomic_get(&peer->active) &&
            peer->active_peripheral_nonce == env->payload.session_id) {
            return 0;
        }
        if (peer->pending && peer->challenge_reset_session != 0) {
            if (peer->pending_peripheral_nonce ==
                env->payload.session_id) {
                /*
                 * A half that was already waiting for a challenge can repeat
                 * its HELLO before the immediate reset challenge reaches the
                 * ACK FIFO. Preserve that transcript.
                 */
                return queue_v3_challenge(source);
            }
            /*
             * Do not let a captured HELLO for another boot nonce displace
             * the bounded MIC-failure reset window. If the half genuinely
             * rebooted, its repeated HELLO is accepted after pending expiry.
             */
            return -ESTALE;
        }
        return begin_v3_pending_session(source, env->payload.session_id, 0, 0);

    case ESB_WIRE_EVENT_V3_RECOVERY: {
        if (env->payload.sequence == 0 || env->payload.session_id == 0) {
            return -EINVAL;
        }
        int recovery_err = accept_v3_recovery_sequence(
            source, peer, env->payload.session_id, env->payload.sequence);
        if (recovery_err != 0) {
            return recovery_err;
        }
        if (atomic_get(&peer->active) &&
            peer->active_peripheral_nonce == env->payload.session_id &&
            peer->active_session ==
                env->payload.body.recovery.active_session) {
            confirm_v3_active_session(source);
            k_mutex_lock(&command_mutex, K_FOREVER);
            bool downlink_needs_rekey =
                peer->down_sequence >= UINT32_MAX - 1U;
            k_mutex_unlock(&command_mutex);
            if (downlink_needs_rekey) {
                return begin_or_repeat_v3_recovery_session(
                    source, env->payload.session_id,
                    env->payload.sequence);
            }
            totem_esb_peer_seen(source);
            totem_esb_benchmark_link_metric(
                source, peer->active_session,
                env->payload.body.recovery.link_metric.metric,
                env->payload.body.recovery.link_metric.value);
            return 0;
        }
        if (atomic_get(&peer->active)) {
            /*
             * The root packet is fresh, but it describes a traffic session
             * that has already been retired. Consume its root sequence and
             * drop it; allowing it to prepare another pending key would turn
             * delayed old-session traffic into a rekey loop.
             */
            return -ESTALE;
        }
        if (!atomic_get(&peer->active) && peer->pending &&
            peer->challenge_reset_session != 0 &&
            peer->challenge_reset_session ==
                env->payload.body.recovery.active_session &&
            peer->pending_peripheral_nonce == env->payload.session_id) {
            /*
             * Do not replace an immediate MIC-failure recovery transcript
             * with a second pending key if the half's old-session heartbeat
             * reaches us before its ACK payload challenge does.
             */
            return queue_v3_challenge(source);
        }
        if (peer->pending && peer->challenge_reset_session != 0 &&
            peer->pending_peripheral_nonce == env->payload.session_id) {
            /*
             * A different retired-session claim under the same boot nonce
             * must not replace an in-progress MIC-failure reset transcript.
             */
            return -ESTALE;
        }
        if (peer->pending && peer->challenge_reset_session == 0 &&
            peer->pending_peripheral_nonce != env->payload.session_id) {
            /*
             * A root-authenticated HELLO for a new boot nonce already owns
             * this handshake. Do not let a captured RECOVERY from the prior
             * boot replace it while READY is in flight.
             */
            return -ESTALE;
        }
        if (peer->pending && peer->challenge_reset_session == 0) {
            return queue_v3_challenge(source);
        }
        return begin_v3_pending_session(source, env->payload.session_id,
                                        env->payload.sequence, 0);
    }

    case ESB_WIRE_EVENT_V3_READY: {
        if (!peer->pending || env->payload.sequence != 0 ||
            env->payload.session_id != peer->pending_session) {
            return -ESTALE;
        }
        bool session_changed = false;
        /*
         * Serialize pending-key promotion with the normal command sender.
         * This keeps an in-flight command from using a key handle while the
         * old active slot is being destroyed, without adding a mutex to the
         * steady-state uplink receive path.
         */
        k_mutex_lock(&command_mutex, K_FOREVER);
        if (!atomic_get(&peer->active) ||
            peer->active_session != peer->pending_session) {
            drop_source_commands(source);
        }
        int err = enqueue_v3_downlink(
            source, ESB_WIRE_COMMAND_V3_SESSION_OK, NULL,
            peer->pending_session, 0, 0, 0, 0);
        if (err != 0) {
            k_mutex_unlock(&command_mutex);
            return err;
        }
        if (!atomic_get(&peer->active) ||
            peer->active_peripheral_nonce !=
                peer->pending_peripheral_nonce ||
            peer->active_central_nonce != peer->central_nonce) {
            atomic_set(&peer->active, false);
            err = totem_esb_v3_activate_pending(source);
            if (err != 0) {
                k_mutex_unlock(&command_mutex);
                return err;
            }
            peer->active_session = peer->pending_session;
            peer->active_peripheral_nonce =
                peer->pending_peripheral_nonce;
            peer->active_central_nonce = peer->central_nonce;
            if (peer->recovery_peripheral_nonce !=
                peer->pending_peripheral_nonce) {
                /*
                 * READY for a HELLO-authenticated new boot nonce starts a new
                 * root sequence space. Same-boot traffic rekeys preserve the
                 * existing high-water mark established by RECOVERY.
                 */
                peer->recovery_peripheral_nonce =
                    peer->pending_peripheral_nonce;
                peer->last_recovery_sequence = peer->request_sequence;
            }
            peer->down_sequence = 0;
            atomic_set(&peer->active, true);
            session_changed = true;
        }
        k_mutex_unlock(&command_mutex);
        if (session_changed) {
            update_source_session(source, peer->active_session, true);
        }
        /*
         * A valid duplicate READY proves that the half is alive even if its
         * SESSION_OK ACK payload has not arrived yet. Keep the promoted key
         * through that bounded retry window.
         */
        totem_esb_peer_seen(source);
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}
#endif

static void process_rx_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    for (int pipe = 1; pipe <= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT; pipe++) {
        struct ring_buf *rx_buf = &state.rx_bufs[pipe];
        while (ring_buf_size_get(rx_buf) > ESB_MSG_WIRE_MIN_SIZE) {
            struct esb_event_envelope env = {0};
            int item_err = zmk_split_esb_get_item(rx_buf, (uint8_t *)&env,
                                                  sizeof(struct esb_event_envelope),
                                                  false, pipe);
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
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                if (env.payload.wire_type == ESB_WIRE_EVENT_V3_HELLO ||
                    env.payload.wire_type == ESB_WIRE_EVENT_V3_RECOVERY ||
                    env.payload.wire_type == ESB_WIRE_EVENT_V3_READY) {
                    int control_err = process_v3_control_event(source, &env);
                    if (control_err == 0) {
                        /* Count authenticated, accepted handshake receptions. */
                        if (env.payload.wire_type == ESB_WIRE_EVENT_V3_HELLO) {
                            totem_esb_diag_event(TOTEM_DIAG_HELLO, 0);
                        } else if (env.payload.wire_type == ESB_WIRE_EVENT_V3_READY) {
                            totem_esb_diag_event(TOTEM_DIAG_READY, 0);
                        }
                    }
                    if (control_err != 0 && control_err != -EALREADY) {
                        totem_esb_benchmark_rx_invalid(pipe, control_err);
                    }
                    break;
                }
                if (!atomic_get(&secure_peers[source].active) ||
                    env.payload.session_id !=
                        secure_peers[source].active_session ||
                    env.payload.sequence == 0) {
                    totem_esb_benchmark_security_drop(
                        source, "session", env.payload.sequence);
                    break;
                }
#else
                update_source_session(source, env.payload.session_id, false);
#endif
                struct esb_rx_sequence_state *seq = &rx_sequences[source];
                uint32_t gap = 0;
                bool accept = true;

                if (seq->initialized) {
                    if (env.payload.sequence == seq->last) {
                        seq->duplicates++;
                        accept = false;
                    } else if (env.payload.sequence > seq->last) {
                        gap = env.payload.sequence - seq->last - 1U;
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

#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                if (accept) {
                    confirm_v3_active_session(source);
                    totem_esb_peer_seen(source);
                } else {
                    totem_esb_benchmark_security_drop(
                        source, "replay", env.payload.sequence);
                }
#else
                totem_esb_peer_seen(source);
#endif

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
                    if (accept && env.payload.wire_type == ESB_WIRE_EVENT_KEY_STATE &&
                        &esb_central == active_transport) {
                        totem_esb_key_state_reconcile(
                            key_pos_states[source], env.payload.body.key_state.keys,
                            CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX,
                            emit_snapshot_key, &source);
                    }
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
            case -EACCES:
                totem_esb_benchmark_rx_invalid(pipe, item_err);
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)
                /*
                 * The clear header is not trusted after CCM rejects its tag.
                 * common.c has already constrained source to the RX pipe and
                 * only returns EACCES after a matching key lookup.
                 * Root-key HELLO/RECOVERY failures are pre-session probes and
                 * are dropped without letting unauthenticated traffic tear
                 * down an otherwise healthy active link.
                 */
                if (env.payload.source > 0 &&
                    env.payload.source <=
                        CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT &&
                    pipe == env.payload.source &&
                    env.payload.wire_type != ESB_WIRE_EVENT_V3_HELLO &&
                    env.payload.wire_type != ESB_WIRE_EVENT_V3_RECOVERY) {
                    uint8_t source = env.payload.source - 1U;
                    LOG_ERR("ESB v3 MIC failure terminated source %u session",
                            source);
                    restart_v3_after_auth_failure(source, &env);
                }
#endif
                break;
            case -EPROTO:
            case -EMSGSIZE:
            case -EBADMSG:
            case -EINVAL:
            case -ENOENT:
            case -EADDRNOTAVAIL:
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
