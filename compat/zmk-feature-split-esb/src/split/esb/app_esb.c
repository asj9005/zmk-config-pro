/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_esb.h"
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <esb.h>

#include <zmk/events/activity_state_changed.h>
#include <totem/esb_benchmark.h>
#include <totem/esb_prx_queue.h>
#include <limits.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
#include "timeslot.h"
static void on_timeslot_start_stop(zmk_split_esb_timeslot_callback_type_t type);
#endif

#define DT_DRV_COMPAT zmk_esb_split
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define HAS_BASE_ADDR_0 (DT_INST_NODE_HAS_PROP(0, base_addr_0))
#define HAS_BASE_ADDR_1 (DT_INST_NODE_HAS_PROP(0, base_addr_1))
#define HAS_ADDR_PREFIX (DT_INST_NODE_HAS_PROP(0, addr_prefix))

#define BASE_ADDR_0_LEN (DT_INST_PROP_LEN(0, base_addr_0))
#define BASE_ADDR_1_LEN (DT_INST_PROP_LEN(0, base_addr_1))
#define ADDR_PREFIX_LEN (DT_INST_PROP_LEN(0, addr_prefix))

#if (!HAS_BASE_ADDR_0 || BASE_ADDR_0_LEN != 4)
#error "zmk,esb-split :: base-addr-0 must include 4 bytes"
#endif

#if (!HAS_BASE_ADDR_1 || BASE_ADDR_1_LEN != 4)
#error "zmk,esb-split :: base-addr-1 must include 4 bytes"
#endif

#if (!HAS_ADDR_PREFIX || ADDR_PREFIX_LEN != CONFIG_ESB_PIPE_COUNT)
#error "zmk,esb-split :: addr-prefix count must equal CONFIG_ESB_PIPE_COUNT"
#endif

uint8_t esb_base_addr_0[BASE_ADDR_0_LEN] = DT_INST_PROP(0, base_addr_0);
uint8_t esb_base_addr_1[BASE_ADDR_1_LEN] = DT_INST_PROP(0, base_addr_1);
uint8_t esb_addr_prefix[ADDR_PREFIX_LEN] = DT_INST_PROP(0, addr_prefix);

#else
#error "Need to create a node with compatible of 'zmk,esb-split` with `all `address` property set."
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP)

#include <zephyr/sys/util_macro.h>

#define RF_CH_IDX(n, _) (CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP_CH_MIN + (n) * CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP_CH_STEP)
static uint8_t esb_rf_ch[CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP_CH_COUNT] = {
    LISTIFY(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP_CH_COUNT, RF_CH_IDX, (,))
};

static uint8_t esb_rf_ch_idx;

uint8_t esb_rf_ch_hop() {
    esb_rf_ch_idx = ++esb_rf_ch_idx % ARRAY_SIZE(esb_rf_ch);
    LOG_DBG("rf ch: %d", esb_rf_ch[esb_rf_ch_idx]);
    esb_set_rf_channel(esb_rf_ch[esb_rf_ch_idx]);
    return esb_rf_ch[esb_rf_ch_idx];
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP) */

static app_esb_callback_t m_callback;

// Track msgq full errors
static uint32_t m_msgq_full_last_time;
static struct k_spinlock m_tx_lock;

// Retry table for tracking application retry rounds and total hardware attempts.
struct retry_entry {
    uint16_t msg_id;
    uint16_t attempts;
    uint8_t left;
};
/*
 * One entry can be in the hardware FIFO while the whole application msgq is
 * occupied, hence the extra slot.
 */
static struct retry_entry m_retry_table[CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS + 1];
static uint16_t m_current_tx_msg_id;

static void clear_retry_table(void) {
    for (int i = 0; i < ARRAY_SIZE(m_retry_table); i++) {
        struct retry_entry *entry = &m_retry_table[i];
        entry->msg_id = 0;
        entry->attempts = 0;
        entry->left = 0;
    }
}

static int find_retry_by_msg_id(uint16_t msg_id) {
    for (int i = 0; i < ARRAY_SIZE(m_retry_table); i++) {
        if (m_retry_table[i].msg_id == msg_id) {
            return i;
        }
    }
    return -1;
}

static int find_empty_retry_slot(void) {
    return find_retry_by_msg_id(0);
}

static int add_retry_entry(uint16_t msg_id, uint8_t max) {
    int idx = find_empty_retry_slot();
    if (idx >= 0) {
        struct retry_entry *entry = &m_retry_table[idx];
        entry->msg_id = msg_id;
        entry->attempts = 0;
        entry->left = max;
    }
    return idx;
}

static void remove_retry_entry_by_msg_id(uint16_t msg_id) {
    int idx = find_retry_by_msg_id(msg_id);
    if (idx >= 0) {
        struct retry_entry *entry = &m_retry_table[idx];
        entry->msg_id = 0;
        entry->attempts = 0;
        entry->left = 0;
    }
}

static uint16_t add_tx_attempts_by_msg_id(uint16_t msg_id, uint16_t attempts) {
    int idx = find_retry_by_msg_id(msg_id);
    if (idx < 0) {
        return attempts;
    }

    uint32_t total = m_retry_table[idx].attempts + attempts;
    m_retry_table[idx].attempts = MIN(total, UINT16_MAX);
    return m_retry_table[idx].attempts;
}

static uint8_t get_retry_left_by_msg_id(uint16_t msg_id) {
    int idx = find_retry_by_msg_id(msg_id);
    return (idx >= 0) ? m_retry_table[idx].left : 0;
}

static uint8_t decrement_retry_by_msg_id(uint16_t msg_id) {
    int idx = find_retry_by_msg_id(msg_id);
    if (idx >= 0 && m_retry_table[idx].left > 0) {
        m_retry_table[idx].left--;
    }
    return (idx >= 0) ? m_retry_table[idx].left : 0;
}

struct queued_payload {
    struct esb_payload payload;
    uint16_t msg_id;
};

// Define a buffer of payloads to store TX payloads in between timeslots
K_MSGQ_DEFINE(m_msgq_tx_payloads, sizeof(struct queued_payload),
              CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS, 4);

static app_esb_mode_t m_mode;
static bool m_active = false;
static bool m_enabled = false;

/* One hardware ACK per pipe leaves room for the other keyboard half. */
BUILD_ASSERT(CONFIG_ESB_TX_FIFO_SIZE >= CONFIG_ESB_PIPE_COUNT,
             "The ACK FIFO must reserve one slot for each ESB pipe");
static struct totem_esb_prx_queue m_prx_queue;
static struct totem_esb_prx_packet m_prx_hardware[CONFIG_ESB_PIPE_COUNT];
static bool m_prx_hardware_valid[CONFIG_ESB_PIPE_COUNT];
static bool m_prx_flush_pending[CONFIG_ESB_PIPE_COUNT];
static void prx_maintenance_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(m_prx_maintenance, prx_maintenance_handler);

static struct onoff_manager *m_hf_manager;
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
static struct onoff_client m_hf_client;
static bool m_hf_ready;
static bool m_hf_requested;
static bool m_hf_pending;
static uint32_t m_hf_idle_since;
static atomic_t m_hf_result = ATOMIC_INIT(INT_MIN);
static void hf_clock_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(m_hf_work, hf_clock_work_handler);
#endif

static int pull_packet_from_tx_msgq(void);
static int pull_packet_from_tx_msgq_unlocked(void);

static void schedule_hf_idle_unlocked(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    if (m_mode == APP_ESB_MODE_PTX && m_current_tx_msg_id == 0 &&
        k_msgq_num_used_get(&m_msgq_tx_payloads) == 0) {
        m_hf_idle_since = k_uptime_get_32();
        k_work_reschedule(&m_hf_work, K_MSEC(CONFIG_TOTEM_ESB_HF_IDLE_MS));
    }
#endif
}

static int service_prx_unlocked(void) {
    uint32_t now = k_uptime_get_32();
    uint32_t blocked = 0;
    bool pending = false;
    unsigned int expired =
        totem_esb_prx_expire(&m_prx_queue, now, CONFIG_TOTEM_ESB_ACK_TTL_MS);

    for (uint8_t pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        int count = esb_get_ack_payload_count(pipe);
        if (count < 0) {
            return count;
        }
        if (count == 0) {
            m_prx_hardware_valid[pipe] = false;
        }
        bool stale = m_prx_hardware_valid[pipe] &&
            totem_esb_prx_expired(now, m_prx_hardware[pipe].enqueued_at,
                                  CONFIG_TOTEM_ESB_ACK_TTL_MS);
        if (m_prx_flush_pending[pipe] || stale) {
            int err = esb_flush_ack_payloads(pipe);
            if (err != 0) {
                m_prx_flush_pending[pipe] = true;
                blocked |= BIT(pipe);
                pending = true;
                continue;
            }
            expired += stale ? 1U : 0U;
            m_prx_hardware_valid[pipe] = false;
            m_prx_flush_pending[pipe] = false;
            count = 0;
        }
        if (count != 0) {
            blocked |= BIT(pipe);
            pending = true;
        }
    }

    int pipe;
    while ((pipe = totem_esb_prx_next(&m_prx_queue, blocked)) >= 0) {
        const struct totem_esb_prx_packet *packet =
            totem_esb_prx_peek(&m_prx_queue, pipe);
        struct esb_payload payload = {.pipe = pipe, .length = packet->length};
        memcpy(payload.data, packet->data, packet->length);
        int err = esb_write_payload(&payload);
        if (err != 0) {
            pending = true;
            break;
        }
        m_prx_hardware[pipe] = *packet;
        m_prx_hardware_valid[pipe] = true;
        totem_esb_prx_pop(&m_prx_queue, pipe);
        blocked |= BIT(pipe);
        pending = true;
    }
    if (expired != 0) {
        LOG_WRN("Expired %u undelivered ESB ACK commands", expired);
    }
    for (uint8_t p = 0; p < CONFIG_ESB_PIPE_COUNT; p++) {
        pending |= m_prx_queue.count[p] != 0;
    }
    if (pending) {
        /* Sustained traffic must not move the expiry deadline into the future. */
        k_work_schedule(&m_prx_maintenance, K_MSEC(25));
    }
    return 0;
}

static void prx_maintenance_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    bool active = m_active && m_mode == APP_ESB_MODE_PRX;
    if (active) {
        service_prx_unlocked();
    }
    k_spin_unlock(&m_tx_lock, key);
    if (active) {
        /* Match the serialization of the normal ESB IRQ callback on nRF52. */
        unsigned int irq_key = irq_lock();
        app_esb_event_t event = {.evt_type = APP_ESB_EVT_TX_SPACE_AVAILABLE};
        m_callback(&event);
        irq_unlock(irq_key);
    }
}

static int pull_packet_from_tx_msgq(void) {
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    int ret = pull_packet_from_tx_msgq_unlocked();
    k_spin_unlock(&m_tx_lock, key);
    return ret;
}

static void event_handler(struct esb_evt const *event) {
    app_esb_event_t m_event = {0};
    switch (event->evt_id) {
        case ESB_EVENT_TX_SUCCESS:
            // LOG_DBG("TX SUCCESS, tx_attempts: %d", event->tx_attempts);
            // LOG_DBG("give d1");
            if (m_mode == APP_ESB_MODE_PRX) {
                // ACK payloads have no application ID/pipe in the SDK event.
                // The event only means a hardware ACK slot became available.
                k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
                pull_packet_from_tx_msgq_unlocked();
                k_spin_unlock(&m_tx_lock, key);
                /*
                 * A previously full lower message queue may now have room.
                 * Ask common.c to retry the preserved producer-ring head.
                 */
                m_event.evt_type = APP_ESB_EVT_TX_SPACE_AVAILABLE;
                m_callback(&m_event);
                break;
            }
            k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
            // Clear retry entry for the PTX message that succeeded.
            m_event.msg_id = m_current_tx_msg_id;
            m_event.tx_attempts =
                add_tx_attempts_by_msg_id(m_current_tx_msg_id, event->tx_attempts);
            remove_retry_entry_by_msg_id(m_current_tx_msg_id);
            m_current_tx_msg_id = 0;
            pull_packet_from_tx_msgq_unlocked();
            schedule_hf_idle_unlocked();
            k_spin_unlock(&m_tx_lock, key);
            // Forward an event to the application
            m_event.evt_type = APP_ESB_EVT_TX_SUCCESS;
            m_callback(&m_event);
            break;
        case ESB_EVENT_TX_FAILED: {
            // LOG_WRN("TX FAILED, tx_attempts: %d", event->tx_attempts);
            if (m_mode == APP_ESB_MODE_PRX) {
                LOG_WRN("Unexpected TX failure while queueing PRX ACK payload");
                k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
                pull_packet_from_tx_msgq_unlocked();
                k_spin_unlock(&m_tx_lock, key);
                break;
            }
            k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
            uint16_t failed_msg_id = m_current_tx_msg_id;
            uint16_t total_attempts =
                add_tx_attempts_by_msg_id(failed_msg_id, event->tx_attempts);
            uint8_t retry_left = get_retry_left_by_msg_id(failed_msg_id);

            /*
             * The Nordic SDK leaves the failed payload, including its RF PID,
             * at the head of its TX FIFO. Restart that same entry immediately
             * so a newer sequence cannot overtake it and PRX duplicate/ACK
             * payload semantics remain intact.
             */
            if (retry_left > 0) {
                decrement_retry_by_msg_id(failed_msg_id);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP)
                esb_rf_ch_hop();
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP) */
                int retry_err = esb_start_tx();
                if (retry_err == 0) {
                    LOG_WRN("Retrying msg %u immediately (%u application retries left)",
                            failed_msg_id, retry_left - 1U);
                    k_spin_unlock(&m_tx_lock, key);
                    break;
                }
                LOG_ERR("Immediate retry failed for msg %u (err %d)", failed_msg_id,
                        retry_err);
            }

            // The retry budget is exhausted, or an immediate restart failed.
            esb_flush_tx();
            m_event.msg_id = failed_msg_id;
            m_event.tx_attempts = total_attempts;
            remove_retry_entry_by_msg_id(failed_msg_id);
            m_current_tx_msg_id = 0;
            pull_packet_from_tx_msgq_unlocked();
            schedule_hf_idle_unlocked();
            k_spin_unlock(&m_tx_lock, key);
            m_event.evt_type = APP_ESB_EVT_TX_FAIL;
            m_callback(&m_event);
            break;
        }
        case ESB_EVENT_RX_RECEIVED:
            // LOG_DBG("RX SUCCESS");
            struct esb_payload rx_payload;
            while (esb_read_rx_payload(&rx_payload) == 0) {
                // LOG_DBG("Chunk %d, pipe: %d, len: %d",
                //     rx_payload.pid, rx_payload.pipe, rx_payload.length);
                uint8_t buf[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
                memcpy(buf, rx_payload.data, rx_payload.length);
                // LOG_DBG("Packet len: %d", rx_payload.length);
                // LOG_HEXDUMP_INF(buf, rx_payload.length, "rx_payload");
                m_event.evt_type = APP_ESB_EVT_RX;
                m_event.pipe = rx_payload.pipe;
                m_event.buf = buf;
                m_event.data_length = rx_payload.length;
                m_callback(&m_event);
            }
            break;
    }
}

static int clocks_start(void) {
    int err;
    int res;
    struct onoff_client clk_cli = {0};

    /* Boot-only wait. Runtime PTX clock requests use the callback below. */
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    m_hf_manager = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!m_hf_manager) {
        LOG_ERR("Unable to get the Clock manager");
        return -ENXIO;
    }

    sys_notify_init_spinwait(&clk_cli.notify);

    err = onoff_request(m_hf_manager, &clk_cli);
    if (err < 0) {
        LOG_ERR("Clock request failed: %d", err);
        return err;
    }

    do {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res) {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err);

    LOG_DBG("HF clock started");
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    m_hf_ready = true;
    m_hf_requested = true;
#endif
    return 0;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
static void hf_clock_ready(struct onoff_manager *manager, struct onoff_client *client,
                           uint32_t state, int result) {
    ARG_UNUSED(manager);
    ARG_UNUSED(client);
    if ((state & ONOFF_FLAG_ERROR) && result >= 0) {
        result = -EIO;
    }
    atomic_set(&m_hf_result, result);
    k_work_reschedule(&m_hf_work, K_NO_WAIT);
}

static void hf_clock_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool request = false;
    bool release = false;
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    if (m_mode != APP_ESB_MODE_PTX) {
        k_spin_unlock(&m_tx_lock, key);
        return;
    }
    if (m_hf_pending) {
        int result = atomic_get(&m_hf_result);
        if (result == INT_MIN) {
            k_spin_unlock(&m_tx_lock, key);
            return;
        }
        m_hf_pending = false;
        m_hf_ready = result >= 0;
        m_hf_requested = result >= 0;
        if (result < 0) {
            LOG_ERR("HF clock request failed: %d", result);
            k_spin_unlock(&m_tx_lock, key);
            k_work_reschedule(&m_hf_work, K_MSEC(100));
            return;
        }
    }

    bool queued = k_msgq_num_used_get(&m_msgq_tx_payloads) != 0;
    if (m_active && m_enabled && queued) {
        if (m_hf_ready) {
            pull_packet_from_tx_msgq_unlocked();
        } else {
            m_hf_pending = true;
            atomic_set(&m_hf_result, INT_MIN);
            request = true;
        }
    } else if (m_hf_requested && m_hf_ready &&
               m_current_tx_msg_id == 0 && esb_is_idle()) {
        uint32_t elapsed = k_uptime_get_32() - m_hf_idle_since;
        if (!m_enabled || elapsed >= CONFIG_TOTEM_ESB_HF_IDLE_MS) {
            /* Senders observe not-ready before the request is released. */
            m_hf_ready = false;
            m_hf_requested = false;
            release = true;
        } else {
            k_work_reschedule(&m_hf_work,
                K_MSEC(CONFIG_TOTEM_ESB_HF_IDLE_MS - elapsed));
        }
    }
    k_spin_unlock(&m_tx_lock, key);

    /* Never wait for a clock, or call a possibly synchronous callback, locked. */
    if (release) {
        int err = onoff_release(m_hf_manager);
        if (err < 0) {
            LOG_ERR("HF clock release failed: %d", err);
        }
    }
    if (request) {
        sys_notify_init_callback(&m_hf_client.notify, hf_clock_ready);
        int err = onoff_request(m_hf_manager, &m_hf_client);
        if (err < 0) {
            atomic_set(&m_hf_result, err);
            k_work_reschedule(&m_hf_work, K_MSEC(100));
        }
    }
}
#endif

static int esb_initialize(app_esb_mode_t mode) {
    int err;
    struct esb_config config = ESB_DEFAULT_CONFIG;

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.retransmit_delay = CONFIG_ZMK_SPLIT_ESB_PROTO_TX_RETRANSMIT_DELAY;
    config.retransmit_count = CONFIG_ZMK_SPLIT_ESB_PROTO_TX_RETRANSMIT_COUNT;
    config.bitrate = ESB_BITRATE_2MBPS;
    config.use_fast_ramp_up = true;
    config.event_handler = event_handler;
    config.mode = (mode == APP_ESB_MODE_PTX) ? ESB_MODE_PTX : ESB_MODE_PRX;
    config.tx_mode = ESB_TXMODE_MANUAL_START;
    config.selective_auto_ack = true;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_CTLR_TX_PWR_PLUS_8)
    config.tx_output_power = ESB_TX_POWER_8DBM;
#endif

    err = esb_init(&config);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP)
    esb_set_rf_channel(esb_rf_ch[esb_rf_ch_idx]);
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP) */

    if (err) {
        return err;
    }

    err = esb_set_base_address_0(esb_base_addr_0);
    if (err) {
        return err;
    }

    err = esb_set_base_address_1(esb_base_addr_1);
    if (err) {
        return err;
    }

    err = esb_set_prefixes(esb_addr_prefix, ARRAY_SIZE(esb_addr_prefix));
    if (err) {
        return err;
    }

    /* Keep the SDK's Zephyr priority mapping: raw NVIC priority 0 bypasses
     * BASEPRI/irq_lock and corrupts the driver's FIFO and event critical sections.
     */

    if (mode == APP_ESB_MODE_PRX) {
        err = esb_start_rx();
        if (err) {
            return err;
        }
    }

    return 0;
}

static int pull_packet_from_tx_msgq_unlocked(void) {
    int ret = 0;
    int esb_ret;
    struct queued_payload queued;

    if (m_mode == APP_ESB_MODE_PRX) {
        return service_prx_unlocked();
    }

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    if (!m_hf_ready) {
        if (k_msgq_num_used_get(&m_msgq_tx_payloads) != 0) {
            k_work_reschedule(&m_hf_work, K_NO_WAIT);
        }
        return -EAGAIN;
    }
#endif
    if (!esb_is_idle()) {
        return -EBUSY;
    }

    if (k_msgq_peek(&m_msgq_tx_payloads, &queued) == 0) {
        struct esb_payload *tx_payload = &queued.payload;
        m_current_tx_msg_id = queued.msg_id;
        ret = esb_write_payload(tx_payload);

        if (ret == -ENOMEM) {
            /*
             * PTX is idle, so a full hardware FIFO is stale state. Flush it
             * once, but preserve the software-queue head if the rewrite still
             * fails.
             */
            esb_flush_tx();
            ret = esb_write_payload(tx_payload);
            if (ret != 0) {
                m_current_tx_msg_id = 0;
                return ret;
            }
        }
        if (ret) {
            LOG_WRN("esb_write_payload failed (%d)", ret);
            m_current_tx_msg_id = 0;
            return ret;
        } else {
            // LOG_DBG("Payload len: %d", tx_payload.length);
            esb_ret = esb_start_tx();
            if (esb_ret < 0) {
                LOG_ERR("esb_start_tx failed (%d)", esb_ret);
                esb_flush_tx();
                m_current_tx_msg_id = 0;
                return esb_ret;
            }
            k_msgq_get(&m_msgq_tx_payloads, &queued, K_NO_WAIT);
            // LOG_INF("TX evt_msg_id: %d", m_current_tx_msg_id);
        }
    }

    return ret;
}

int zmk_split_esb_init(app_esb_mode_t mode, app_esb_callback_t callback) {
    int ret;
    m_callback = callback;
    m_mode = mode;
    totem_esb_prx_queue_init(&m_prx_queue);
    ret = clocks_start();
    if (ret < 0) {
        return ret;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    LOG_INF("Timeslothandler init");
    zmk_split_esb_timeslot_init(on_timeslot_start_stop);
    return 0;
#else
    ret = zmk_split_esb_set_enable(true);
    if (ret) {
        LOG_ERR("esb enable failed: %d", ret);
    }
    return ret;
#endif
}

int zmk_split_esb_set_enable(bool enabled) {
    m_enabled = enabled;
    if (enabled) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
        zmk_split_esb_timeslot_open_session();
#else
        int ret = esb_initialize(m_mode);
        if (ret) {
            LOG_ERR("set_enable: esb_initialize failed: %d", ret);
            m_enabled = false;
            return ret;
        }
        m_active = true;
        if (m_mode == APP_ESB_MODE_PTX) {
            clear_retry_table();
            m_current_tx_msg_id = 0;
        }
        pull_packet_from_tx_msgq();
        k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
        schedule_hf_idle_unlocked();
        k_spin_unlock(&m_tx_lock, key);
#endif
        return 0;
    } else {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
        zmk_split_esb_timeslot_close_session();
#else
        m_active = false;
        esb_disable();
        k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
        schedule_hf_idle_unlocked();
        k_spin_unlock(&m_tx_lock, key);
#endif
        return 0;
    }
}

int zmk_split_esb_send(app_esb_data_t *tx_packet) {
    if (tx_packet == NULL || tx_packet->data == NULL) {
        return -EINVAL;
    }
    if (tx_packet->len == 0 || tx_packet->len > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    if (tx_packet->pipe >= CONFIG_ESB_PIPE_COUNT) {
        return -EINVAL;
    }

    if (m_mode == APP_ESB_MODE_PRX) {
        struct totem_esb_prx_packet packet = {
            .pipe = tx_packet->pipe, .length = tx_packet->len,
            .msg_id = tx_packet->msg_id, .enqueued_at = k_uptime_get_32(),
        };
        memcpy(packet.data, tx_packet->data, packet.length);
        k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
        if (m_active) {
            service_prx_unlocked();
        }
        bool duplicate = m_prx_hardware_valid[packet.pipe] &&
            !m_prx_flush_pending[packet.pipe] &&
            totem_esb_prx_same(&packet, &m_prx_hardware[packet.pipe]);
        int ret = duplicate ? 1 : totem_esb_prx_offer(&m_prx_queue, &packet);
        if (m_active) {
            service_prx_unlocked();
        }
        k_spin_unlock(&m_tx_lock, key);
        return ret >= 0 ? 0 : (ret == -ENOSPC ? -ENOMSG : ret);
    }

    int ret = 0;
    struct queued_payload queued = {.msg_id = tx_packet->msg_id};
    struct esb_payload *tx_payload = &queued.payload;
    tx_payload->pipe = tx_packet->pipe;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_PROTO_TX_ACK)
    tx_payload->noack = false;
#else
    tx_payload->noack = true;
#endif
    memcpy(tx_payload->data, tx_packet->data, tx_packet->len);
    tx_payload->length = tx_packet->len;
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    ret = k_msgq_put(&m_msgq_tx_payloads, &queued, K_NO_WAIT);

    if (ret == 0) {
        if (m_mode == APP_ESB_MODE_PTX) {
            add_retry_entry(tx_packet->msg_id, tx_packet->max_retry);
        }
        m_msgq_full_last_time = 0;
    } else if (ret == -ENOMSG) {
        totem_esb_transport_queue_pressure(false);
        uint32_t now = k_uptime_get_32();
        if (!m_msgq_full_last_time) {
            m_msgq_full_last_time = now;
        }
        if (now - m_msgq_full_last_time > CONFIG_ZMK_SPLIT_ESB_MSGQ_FULL_TIMEOUT_MS) {
            LOG_WRN("Msgq has remained full for at least %dms; preserving queued packets",
                    CONFIG_ZMK_SPLIT_ESB_MSGQ_FULL_TIMEOUT_MS);
            m_msgq_full_last_time = now;
        }
    } else {
        LOG_DBG("Failed to queue esb tx_payload_q (%d)", ret);
    }
    if (m_active) {
        pull_packet_from_tx_msgq_unlocked();
    }
    k_spin_unlock(&m_tx_lock, key);
    return ret;
}

int zmk_split_esb_flush_pipe(uint8_t pipe) {
    if (m_mode != APP_ESB_MODE_PRX || pipe >= CONFIG_ESB_PIPE_COUNT) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    totem_esb_prx_clear_pipe(&m_prx_queue, pipe);
    m_prx_hardware_valid[pipe] = false;
    m_prx_flush_pending[pipe] = true;
    int err = m_active ? esb_flush_ack_payloads(pipe) : -EBUSY;
    if (err == 0) {
        m_prx_flush_pending[pipe] = false;
    }
    k_spin_unlock(&m_tx_lock, key);
    /* An in-flight ACK cannot be revoked. Hold new commands behind this barrier. */
    k_work_reschedule(&m_prx_maintenance, K_MSEC(1));
    return err == -EBUSY ? 0 : err;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
static int app_esb_suspend(void) {
    m_active = false;
    if (m_mode == APP_ESB_MODE_PTX) {
        uint32_t irq_key = irq_lock();

        irq_disable(RADIO_IRQn);
        NVIC_DisableIRQ(RADIO_IRQn);

        NRF_RADIO->SHORTS = 0;

        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);

        NRF_TIMER2->TASKS_STOP = 1;
        NRF_RADIO->INTENCLR = 0xFFFFFFFF;
        
        esb_disable();

        NVIC_ClearPendingIRQ(RADIO_IRQn);

        irq_unlock(irq_key);
    }
    else {
        esb_stop_rx();
    }

    // Todo: Figure out how to use the esb_suspend() function
    // rather than having to disable at the end of every timeslot
    //esb_suspend();
    return 0;
}

static int app_esb_resume(void) {
    if (m_mode == APP_ESB_MODE_PTX) {
        int err = esb_initialize(m_mode);
        m_active = true;
        clear_retry_table();
        m_current_tx_msg_id = 0;
        pull_packet_from_tx_msgq();
        return err;
    }
    else {
        int err = esb_initialize(m_mode);
        m_active = true;
        pull_packet_from_tx_msgq();
        return err;
    }
}

/* Callback function signalling that a timeslot is started or stopped */
static void on_timeslot_start_stop(zmk_split_esb_timeslot_callback_type_t type) {
    switch (type) {
        case APP_TS_STARTED:
            app_esb_resume();
            break;
        case APP_TS_STOPPED:
            app_esb_suspend();
            break;
    }
}
#endif

static int on_activity_state(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *state_ev = as_zmk_activity_state_changed(eh);
    if (!state_ev) {
        return 0;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT)
    if (m_mode == APP_ESB_MODE_PTX) {
        if (state_ev->state != ZMK_ACTIVITY_ACTIVE && m_enabled) {
            zmk_split_esb_set_enable(false);
        }
        else if (state_ev->state == ZMK_ACTIVITY_ACTIVE && !m_enabled) {
            zmk_split_esb_set_enable(true);
        }
    }
#endif

    return 0;
}

ZMK_LISTENER(zmk_split_esb_idle_sleeper, on_activity_state);
ZMK_SUBSCRIPTION(zmk_split_esb_idle_sleeper, zmk_activity_state_changed);
