/* Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 * The runner inserts the unchanged SDK structures and function bodies below.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_ESB_PIPE_COUNT 3
#define CONFIG_ESB_TX_FIFO_SIZE 4
#define ESB_MODE_PTX 0
#define ESB_MODE_PRX 1
#define ESB_STATE_PRX 0
#define ESB_STATE_PRX_SEND_ACK 1

static int fake_radio;
#define NRF_RADIO (&fake_radio)

/* The tested routines only compare payload pointers; radio bytes are opaque. */
struct esb_payload { uint8_t marker; };

/* ACTUAL_SDK_STRUCTURES */

static bool esb_initialized;
static struct { int mode; } esb_cfg;
static int esb_state;
static struct payload_wrap nodes[4];
static struct esb_payload payloads[4];
static struct payload_wrap *ack_pl_wrap_pipe[CONFIG_ESB_PIPE_COUNT];
static struct pipe_info rx_pipe_info[CONFIG_ESB_PIPE_COUNT];
static struct payload_tx_fifo tx_fifo;
static struct esb_payload *current_payload;
static unsigned int lock_depth, lock_calls, unlock_calls;
static uint8_t radio_txaddress;

static unsigned int irq_lock(void) {
    assert(lock_depth == 0);
    lock_depth++;
    lock_calls++;
    return 0xA55AU;
}

static void irq_unlock(unsigned int key) {
    assert(key == 0xA55AU && lock_depth == 1);
    lock_depth--;
    unlock_calls++;
}

static uint32_t nrf_radio_txaddress_get(const int *radio) {
    assert(radio == NRF_RADIO && lock_depth == 1U);
    return radio_txaddress;
}

/* ACTUAL_SDK_FUNCTIONS */

static void reset(void) {
    esb_initialized = true;
    esb_cfg.mode = ESB_MODE_PRX;
    esb_state = ESB_STATE_PRX;
    memset(nodes, 0, sizeof(nodes));
    memset(payloads, 0, sizeof(payloads));
    memset(ack_pl_wrap_pipe, 0, sizeof(ack_pl_wrap_pipe));
    memset(rx_pipe_info, 0, sizeof(rx_pipe_info));
    memset(&tx_fifo, 0, sizeof(tx_fifo));
    for (unsigned int i = 0; i < 4U; ++i) {
        nodes[i].p_payload = &payloads[i];
        nodes[i].in_use = true;
        payloads[i].marker = (uint8_t)(i + 1U);
        tx_fifo.payload[i] = &payloads[i];
    }
    nodes[0].p_next = &nodes[1];
    nodes[2].p_next = &nodes[3];
    ack_pl_wrap_pipe[1] = &nodes[0];
    ack_pl_wrap_pipe[2] = &nodes[2];
    rx_pipe_info[1].crc = 0x1234U;
    rx_pipe_info[1].pid = 1U;
    rx_pipe_info[1].ack_payload = true;
    rx_pipe_info[2].crc = 0xABCDU;
    rx_pipe_info[2].pid = 2U;
    rx_pipe_info[2].ack_payload = true;
    tx_fifo.count = 4U;
    tx_fifo.front = 2U;
    tx_fifo.back = 3U;
    current_payload = &payloads[0];
    lock_depth = lock_calls = unlock_calls = 0U;
    radio_txaddress = 1U;
}

static void check_unchanged(void) {
    assert(tx_fifo.count == 4U && tx_fifo.front == 2U && tx_fifo.back == 3U);
    assert(ack_pl_wrap_pipe[0] == NULL);
    assert(ack_pl_wrap_pipe[1] == &nodes[0] && nodes[0].p_next == &nodes[1]);
    assert(ack_pl_wrap_pipe[2] == &nodes[2] && nodes[2].p_next == &nodes[3]);
    assert(nodes[1].p_next == NULL && nodes[3].p_next == NULL);
    for (unsigned int i = 0; i < 4U; ++i) {
        assert(nodes[i].in_use && nodes[i].p_payload == &payloads[i]);
        assert(tx_fifo.payload[i] == &payloads[i]);
    }
    assert(current_payload == &payloads[0]);
    assert(rx_pipe_info[1].crc == 0x1234U && rx_pipe_info[1].pid == 1U);
    assert(rx_pipe_info[2].crc == 0xABCDU && rx_pipe_info[2].pid == 2U);
    assert(rx_pipe_info[1].ack_payload && rx_pipe_info[2].ack_payload);
    assert(lock_depth == 0U && lock_calls == unlock_calls);
}

static void count_only_inspects_target_pipe(void) {
    reset();
    assert(esb_get_ack_payload_count(0U) == 0);
    assert(esb_get_ack_payload_count(1U) == 2);
    assert(esb_get_ack_payload_count(2U) == 2);
    assert(lock_calls == 3U && unlock_calls == 3U);
    check_unchanged();
}

static void invalid_calls_preserve_all_state(void) {
    reset();
    esb_initialized = false;
    assert(esb_get_ack_payload_count(1U) == -EACCES);
    assert(esb_flush_ack_payloads(1U) == -EACCES);
    esb_initialized = true;
    esb_cfg.mode = ESB_MODE_PTX;
    assert(esb_get_ack_payload_count(1U) == -EINVAL);
    assert(esb_flush_ack_payloads(1U) == -EINVAL);
    esb_cfg.mode = ESB_MODE_PRX;
    assert(esb_get_ack_payload_count(CONFIG_ESB_PIPE_COUNT) == -EINVAL);
    assert(esb_flush_ack_payloads(CONFIG_ESB_PIPE_COUNT) == -EINVAL);
    assert(esb_get_ack_payload_count(UINT8_MAX) == -EINVAL);
    assert(esb_flush_ack_payloads(UINT8_MAX) == -EINVAL);
    assert(lock_calls == 0U && unlock_calls == 0U);
    check_unchanged();
}

static void active_ack_cannot_be_cancelled_mid_transmission(void) {
    reset();
    esb_state = ESB_STATE_PRX_SEND_ACK;
    assert(esb_flush_ack_payloads(1U) == -EBUSY);
    assert(esb_flush_ack_payloads(0U) == 0); /* An empty pipe has no active ACK. */
    assert(lock_calls == 2U && unlock_calls == 2U);
    check_unchanged();
}

static void another_pipe_can_be_cancelled_during_active_ack(void) {
    reset();
    esb_state = ESB_STATE_PRX_SEND_ACK;
    assert(esb_flush_ack_payloads(2U) == 0);
    assert(tx_fifo.count == 2U && current_payload == &payloads[0]);
    assert(ack_pl_wrap_pipe[1] == &nodes[0] && nodes[0].p_next == &nodes[1]);
    assert(nodes[0].in_use && nodes[1].in_use && rx_pipe_info[1].ack_payload);
    assert(ack_pl_wrap_pipe[2] == NULL && !nodes[2].in_use && !nodes[3].in_use);
    assert(!rx_pipe_info[2].ack_payload);
    assert(rx_pipe_info[1].crc == 0x1234U && rx_pipe_info[1].pid == 1U);
    assert(rx_pipe_info[2].crc == 0xABCDU && rx_pipe_info[2].pid == 2U);
    assert(lock_depth == 0U && lock_calls == unlock_calls);
}

static void cancelling_one_pipe_preserves_other_ack_and_replay_state(void) {
    reset();
    assert(esb_flush_ack_payloads(1U) == 0);
    assert(lock_calls == 1U && unlock_calls == 1U && lock_depth == 0U);
    assert(ack_pl_wrap_pipe[1] == NULL);
    assert(!nodes[0].in_use && !nodes[1].in_use);
    assert(nodes[0].p_next == NULL && nodes[1].p_next == NULL);
    assert(current_payload == NULL && tx_fifo.count == 2U);
    assert(tx_fifo.front == 2U && tx_fifo.back == 3U);
    assert(!rx_pipe_info[1].ack_payload);
    assert(rx_pipe_info[1].crc == 0x1234U && rx_pipe_info[1].pid == 1U);
    assert(ack_pl_wrap_pipe[2] == &nodes[2] && nodes[2].p_next == &nodes[3]);
    assert(nodes[2].in_use && nodes[3].in_use && nodes[3].p_next == NULL);
    assert(rx_pipe_info[2].ack_payload);
    assert(rx_pipe_info[2].crc == 0xABCDU && rx_pipe_info[2].pid == 2U);
    assert(esb_get_ack_payload_count(1U) == 0 && esb_get_ack_payload_count(2U) == 2);
}

static void unrelated_current_payload_survives_cancellation(void) {
    reset();
    current_payload = &payloads[3];
    assert(esb_flush_ack_payloads(1U) == 0);
    assert(current_payload == &payloads[3] && tx_fifo.count == 2U);
    assert(esb_flush_ack_payloads(2U) == 0);
    assert(current_payload == NULL && tx_fifo.count == 0U);
    assert(!nodes[2].in_use && !nodes[3].in_use);
    assert(nodes[2].p_next == NULL && nodes[3].p_next == NULL);
    assert(!rx_pipe_info[2].ack_payload);
    assert(rx_pipe_info[2].crc == 0xABCDU && rx_pipe_info[2].pid == 2U);
    assert(lock_depth == 0U && lock_calls == unlock_calls);
}

static void repeated_empty_cancellation_never_underflows_count(void) {
    reset();
    assert(esb_flush_ack_payloads(1U) == 0);
    for (unsigned int i = 0; i < 3U; ++i) {
        /* Cancellation also clears a leftover ACK-completion marker. */
        rx_pipe_info[1].ack_payload = true;
        assert(esb_flush_ack_payloads(1U) == 0);
        assert(tx_fifo.count == 2U && !rx_pipe_info[1].ack_payload);
    }
    assert(esb_flush_ack_payloads(0U) == 0);
    assert(esb_get_ack_payload_count(2U) == 2);
    assert(lock_depth == 0U && lock_calls == unlock_calls);
}

int main(void) {
    count_only_inspects_target_pipe();
    invalid_calls_preserve_all_state();
    active_ack_cannot_be_cancelled_mid_transmission();
    another_pipe_can_be_cancelled_during_active_ack();
    cancelling_one_pipe_preserves_other_ack_and_replay_state();
    unrelated_current_payload_survives_cancellation();
    repeated_empty_cancellation_never_underflows_count();
    puts("7 SDK ACK runtime cases passed");
    return 0;
}
