/* Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 * Actual SDK functions are inserted unchanged by the Python runner. Fakes model
 * 1 MHz TIMER compare/short/IRQ ordering and relevant hardware PPI connections.
 * RADIO IRQ servicing is explicitly delayed by test cases; TIMER IRQs are not.
 * This is a host model, not validation of RF, silicon, or interrupt priorities.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s: %s (line %d)\n", __func__, #expr, __LINE__); exit(1); \
} } while (0)
#define BIT(n) (1U << (n))
#define IS_ENABLED(x) (x)
#define CONFIG_ESB_NEVER_DISABLE_TX 0
#define ERRATA_216_PRESENT 0
#define ERRATA_216_ENABLED 1
#define ERRATA_216_MIN_TIME_TO_DISABLE_US 100
#define ERRATA_216_RADIO_ENABLE_DELAY_US 10
#define NRF_TIMER_HAS_SHUTDOWN 1
#define NRF_TIMER_CC_CHANNEL0 0
#define NRF_TIMER_CC_CHANNEL1 1
#define NRF_TIMER_CC_CHANNEL2 2
#define NRF_TIMER_CC_CHANNEL3 3
#define NRF_TIMER_EVENT_COMPARE0 0
#define NRF_TIMER_EVENT_COMPARE1 1
#define NRF_TIMER_EVENT_COMPARE2 2
#define NRF_TIMER_EVENT_COMPARE3 3
#define NRF_TIMER_INT_COMPARE1_MASK BIT(1)
#define NRF_TIMER_INT_COMPARE3_MASK BIT(3)
#define NRF_TIMER_SHORT_COMPARE1_CLEAR_MASK BIT(9)
#define NRF_TIMER_SHORT_COMPARE2_CLEAR_MASK BIT(10)
#define NRF_TIMER_SHORT_COMPARE1_STOP_MASK BIT(13)
#define NRF_TIMER_SHORT_COMPARE2_STOP_MASK BIT(14)
#define NRF_TIMER_TASK_START 0
#define NRF_TIMER_TASK_STOP 1
#define NRF_TIMER_TASK_CLEAR 2
#define NRF_TIMER_TASK_SHUTDOWN 3
#define ESB_PROTOCOL_ESB 0
#define ESB_PROTOCOL_ESB_DPL 1
#define ESB_TXMODE_MANUAL 0
#define ESB_STATE_IDLE 0
#define ESB_STATE_PTX_TX_ACK 1
#define ESB_STATE_PTX_RX_ACK 2
#define ESB_STATE_PTX_TX 3
#define ESB_STATE_PTX_TXIDLE 4
#define INT_TX_SUCCESS_MSK BIT(0)
#define INT_TX_FAILED_MSK BIT(1)
#define INT_RX_DATA_RECEIVED_MSK BIT(2)
#define NRF_RADIO_EVENT_READY 0
#define ESB_RADIO_EVENT_END 1
#define NRF_RADIO_STATE_TXRU 1
#define NRF_RADIO_TASK_RXEN 0
#define NRF_RADIO_SHORT_DISABLED_RXEN_MASK BIT(0)
#define MPSL_FEM_ALL 0
#define ESB_EGU NULL
#define ESB_EGU_TASK 0

typedef int nrf_timer_event_t;
struct timer_model {
    uint32_t counter, cc[4], shorts, interrupts;
    bool configured[4], events[4], running;
};
static struct timer_model timer;
static struct { struct timer_model *p_reg; } esb_timer = {&timer};
struct radio_model { bool events[2], crc_ok; unsigned int state; };
static struct radio_model radio;
#define NRF_RADIO (&radio)
static struct {
    bool use_fast_ramp_up;
    unsigned int retransmit_delay, retransmit_count, protocol, tx_mode;
} esb_cfg;
static struct { struct { struct { struct { uint32_t end; } counter_period; } timer; } event; }
    tx_time_shifted;
static int tx_event, rx_event, disable_event;
static unsigned int esb_state, retransmits_remaining, last_tx_attempts;
static uint32_t interrupt_flags, wait_for_ack_timeout_us, radio_shorts_common;
static bool fast_switching;
static int errata_216_status;
static uint32_t errata_216_timer_shorts;
struct esb_payload { unsigned int length; uint8_t marker; };
static struct esb_payload payload;
static struct esb_payload *current_payload = &payload;
static struct { unsigned int count; } tx_fifo;
struct esb_radio_pdu { struct { struct { uint8_t length, pid; } dpl_pdu; } type; };
static struct esb_radio_pdu rx_pdu_storage, tx_pdu_storage;
#define rx_payload_buffer (&rx_pdu_storage)
#define tx_payload_buffer (&tx_pdu_storage)
static void (*on_radio_disabled)(void);
static void (*on_timer_compare1)(void);
static bool ppi_disabled_start, ppi_wait_ack, ppi_retransmit;
static bool pending_radio_disabled;
static unsigned int radio_disable_count, radio_retry_launches, completion_irqs;
static unsigned int removed_payloads, cc1_callbacks;
static uint32_t now_us;

static void on_radio_disabled_tx(void);
static void on_radio_disabled_tx_wait_for_ack(void);
static void esb_timer_handler(nrf_timer_event_t event_type, void *context);

static void nrf_timer_task_trigger(struct timer_model *dev, int task) {
    CHECK(dev == &timer);
    if (task == NRF_TIMER_TASK_START) dev->running = true;
    else if (task == NRF_TIMER_TASK_STOP) dev->running = false;
    else if (task == NRF_TIMER_TASK_CLEAR) dev->counter = 0;
    else { CHECK(task == NRF_TIMER_TASK_SHUTDOWN); dev->running = false; dev->counter = 0; }
}
static void nrf_timer_shorts_set(struct timer_model *dev, uint32_t mask) { dev->shorts = mask; }
static void nrf_timer_shorts_disable(struct timer_model *dev, uint32_t mask) { dev->shorts &= ~mask; }
static bool nrf_timer_int_enable_check(struct timer_model *dev, uint32_t mask) {
    return (dev->interrupts & mask) != 0;
}
static void nrf_timer_int_disable(struct timer_model *dev, uint32_t mask) { dev->interrupts &= ~mask; }
static uint32_t nrf_timer_compare_int_get(unsigned int channel) { return BIT(channel); }
static void nrf_timer_event_clear(struct timer_model *dev, int event) { dev->events[event] = false; }
static bool nrf_timer_event_check(struct timer_model *dev, int event) { return dev->events[event]; }
static uint32_t nrf_timer_cc_get(struct timer_model *dev, unsigned int channel) { return dev->cc[channel]; }
static uint32_t nrfx_timer_capture_get(const void *dev, unsigned int channel) {
    CHECK(dev == &esb_timer); return timer.cc[channel];
}
static void nrfx_timer_compare(const void *dev, unsigned int channel, uint32_t value, bool irq) {
    CHECK(dev == &esb_timer && channel < 4U);
    timer.cc[channel] = value; timer.configured[channel] = true;
    /* nrfx clears a pre-existing compare event when enabling its interrupt. */
    if (irq) { timer.events[channel] = false; timer.interrupts |= BIT(channel); }
    else timer.interrupts &= ~BIT(channel);
}
static int mpsl_fem_pa_configuration_set(const void *a, const void *b) { return -1; }
static int mpsl_fem_lna_configuration_set(const void *a, const void *b) { return -1; }
static void mpsl_fem_enable(void) {}
static void mpsl_fem_disable(void) {}
static void mpsl_fem_pa_configuration_clear(void) {}
static void mpsl_fem_lna_configuration_clear(void) {}
static void mpsl_fem_deactivate_now(int mask) {}
static void esb_ppi_for_fem_set(void) { ppi_disabled_start = true; }
static void esb_ppi_for_fem_clear(void) { ppi_disabled_start = false; }
static void esb_ppi_for_txrx_clear(bool rx, bool ack) { ppi_disabled_start = false; }
static void esb_ppi_for_txrx_set(bool rx, bool ack) { ppi_disabled_start = ack; }
static void esb_ppi_for_wait_for_ack_set(void) { ppi_wait_ack = true; }
static void esb_ppi_for_wait_for_ack_clear(void) { ppi_wait_ack = false; }
static void esb_ppi_for_retransmission_set(void) { ppi_retransmit = true; }
static void esb_ppi_for_retransmission_clear(void) { ppi_retransmit = false; }
static void esb_ppi_for_wait_for_rx_clear(void) {}
static void nrf_radio_shorts_set(struct radio_model *dev, uint32_t shorts) { (void)dev; (void)shorts; }
static void nrf_radio_packetptr_set(struct radio_model *dev, const void *ptr) { (void)dev; (void)ptr; }
static void nrf_radio_event_clear(struct radio_model *dev, int event) { dev->events[event] = false; }
static bool nrf_radio_event_check(struct radio_model *dev, int event) { return dev->events[event]; }
static bool nrf_radio_crc_status_check(struct radio_model *dev) { return dev->crc_ok; }
static unsigned int nrf_radio_txaddress_get(struct radio_model *dev) { (void)dev; return 1; }
static unsigned int nrf_radio_state_get(struct radio_model *dev) { return dev->state; }
static void nrf_radio_task_trigger(struct radio_model *dev, int task) { (void)dev; CHECK(task == NRF_RADIO_TASK_RXEN); }
static void update_rf_payload_format(unsigned int length) { (void)length; }
static void update_radio_tx_power(void) {}
static void errata216_on(void) {}
static void errata216_off(void) {}
static int atomic_get(const int *value) { return *value; }
static void nrf_egu_task_trigger(const void *dev, int task) { (void)dev; (void)task; }
static void set_evt_interrupt(void) { completion_irqs++; }
static void tx_fifo_remove_last(void) {
    CHECK(tx_fifo.count == 1U); tx_fifo.count--; removed_payloads++;
}
static bool rx_fifo_push_rfbuf(unsigned int pipe, unsigned int pid) { (void)pipe; (void)pid; return true; }
static void start_tx_transaction(void) { CHECK(false); }
static void radio_start(void) {
    radio.state = NRF_RADIO_STATE_TXRU;
    radio_retry_launches++;
    nrf_timer_task_trigger(&timer, NRF_TIMER_TASK_START);
}

/* ACTUAL_SDK_FUNCTIONS */

/* PPI and shorts happen in hardware, before the modeled nrfx timer ISR. The
 * nrfx IRQ wrapper clears the compare event before passing its type to ESB. */
static void advance_us(uint32_t duration) {
    while (duration-- > 0U) {
        now_us++;
        if (!timer.running) continue;
        timer.counter = (timer.counter + 1U) & 0xFFFFU;
        uint32_t matches = 0;
        for (unsigned int channel = 0; channel < 4U; channel++) {
            if (timer.configured[channel] && timer.counter == timer.cc[channel]) {
                timer.events[channel] = true; matches |= BIT(channel);
            }
        }
        if ((matches & BIT(0)) && ppi_wait_ack) {
            pending_radio_disabled = true; radio_disable_count++; radio.state = 0;
        }
        if ((matches & BIT(1)) && ppi_retransmit) {
            radio.state = NRF_RADIO_STATE_TXRU; radio_retry_launches++;
        }
        if ((timer.shorts >> 12) & matches) timer.running = false;
        if ((timer.shorts >> 8) & matches) timer.counter = 0;
        for (unsigned int channel = 0; channel < 4U; channel++) {
            if ((matches & timer.interrupts & BIT(channel)) != 0U) {
                timer.events[channel] = false;
                esb_timer_handler((int)channel, NULL);
            }
        }
    }
}
static void reset(void) {
    memset(&timer, 0, sizeof(timer)); memset(&radio, 0, sizeof(radio));
    memset(&rx_pdu_storage, 0, sizeof(rx_pdu_storage));
    esb_cfg.use_fast_ramp_up = true; esb_cfg.retransmit_delay = 500;
    esb_cfg.retransmit_count = 3; esb_cfg.protocol = ESB_PROTOCOL_ESB_DPL;
    esb_cfg.tx_mode = ESB_TXMODE_MANUAL; fast_switching = false;
    wait_for_ack_timeout_us = RX_ACK_TIMEOUT_US_2MBPS;
    esb_state = ESB_STATE_PTX_TX_ACK; retransmits_remaining = esb_cfg.retransmit_count;
    last_tx_attempts = interrupt_flags = radio_shorts_common = 0;
    on_radio_disabled = on_radio_disabled_tx; on_timer_compare1 = NULL;
    ppi_disabled_start = ppi_wait_ack = ppi_retransmit = false;
    pending_radio_disabled = false;
    radio_disable_count = radio_retry_launches = completion_irqs = removed_payloads = cc1_callbacks = 0;
    now_us = 0; tx_fifo.count = 1; payload.length = 32; payload.marker = 0xA5;
}
static void first_ramp(bool ack) {
    esb_ppi_for_txrx_set(false, ack);
    esb_fem_for_tx_set(ack);
    nrf_timer_task_trigger(&timer, NRF_TIMER_TASK_START);
    advance_us(TX_FAST_RAMP_UP_TIME_US);
    CHECK(!timer.running);
    CHECK(timer.counter == (ack ? 0U : TX_FAST_RAMP_UP_TIME_US));
    advance_us(200); /* Transmit completes while the ramp-up timer is stopped. */
}
static void tx_disabled(void) {
    CHECK(ppi_disabled_start);
    nrf_timer_task_trigger(&timer, NRF_TIMER_TASK_START);
    pending_radio_disabled = true;
    radio.state = 0;
}
static void service_radio(void) {
    CHECK(pending_radio_disabled && on_radio_disabled != NULL);
    pending_radio_disabled = false;
    on_radio_disabled();
}

static void delayed_radio_irq(void) {
    reset(); first_ramp(true);
    for (unsigned int attempt = 0; attempt <= esb_cfg.retransmit_count; attempt++) {
        tx_disabled();
        advance_us(60); /* RADIO ISR delayed beyond the stale 40 us STOP compare. */
        CHECK(timer.running && timer.counter == 60U);
        service_radio();
        CHECK(esb_state == ESB_STATE_PTX_RX_ACK);
        advance_us(wait_for_ack_timeout_us + ADDR_EVENT_LATENCY_US - 60U);
        CHECK(pending_radio_disabled && radio_disable_count == attempt + 1U);
        service_radio();
        if (attempt < esb_cfg.retransmit_count) {
            CHECK(esb_state == ESB_STATE_PTX_TX_ACK);
            CHECK(retransmits_remaining == esb_cfg.retransmit_count - attempt - 1U);
            advance_us(esb_cfg.retransmit_delay - TX_FAST_RAMP_UP_TIME_US - timer.counter);
            CHECK(radio_retry_launches == attempt + 1U);
            CHECK(!timer.running && timer.counter == 0U);
            advance_us(200);
        }
    }
    CHECK(esb_state == ESB_STATE_IDLE && !timer.running);
    CHECK(interrupt_flags == INT_TX_FAILED_MSK && completion_irqs == 1U);
    CHECK(last_tx_attempts == 4U && now_us < 3000U);
    CHECK(tx_fifo.count == 1U && removed_payloads == 0U && payload.marker == 0xA5);
}
static void elapsed_ack_deadline(void) {
    reset(); first_ramp(true); tx_disabled(); advance_us(60); service_radio();
    CHECK(timer.counter == 60U);
    uint32_t remaining = wait_for_ack_timeout_us + ADDR_EVENT_LATENCY_US - 60U;
    advance_us(remaining - 1U); CHECK(!pending_radio_disabled);
    advance_us(1); CHECK(pending_radio_disabled);
    CHECK(timer.counter == wait_for_ack_timeout_us + ADDR_EVENT_LATENCY_US);
}
static void healthy_ack_address(void) {
    reset(); first_ramp(true); tx_disabled(); advance_us(20); service_radio();
    advance_us(50); CHECK(ppi_wait_ack && timer.counter == 70U);
    /* The pinned nRF52840 PPI path connects RADIO ADDRESS -> TIMER SHUTDOWN,
     * which stops AND clears the counter, unlike the STOP-only path. */
    nrf_timer_task_trigger(&timer, NRF_TIMER_TASK_SHUTDOWN);
    radio.events[ESB_RADIO_EVENT_END] = true; radio.crc_ok = true;
    pending_radio_disabled = true; service_radio();
    advance_us(1000);
    CHECK(!timer.running && timer.counter == 0U && radio_disable_count == 0U);
    CHECK(esb_state == ESB_STATE_IDLE && interrupt_flags == INT_TX_SUCCESS_MSK);
    CHECK(tx_fifo.count == 0U && removed_payloads == 1U && last_tx_attempts == 1U);
}
static void count_cc1_callback(void) { cc1_callbacks++; }
static void cc1_preservation(void) {
    reset();
    uint32_t cc1_shorts = NRF_TIMER_SHORT_COMPARE1_CLEAR_MASK | NRF_TIMER_SHORT_COMPARE1_STOP_MASK;
    timer.shorts = cc1_shorts | NRF_TIMER_SHORT_COMPARE2_CLEAR_MASK | NRF_TIMER_SHORT_COMPARE2_STOP_MASK;
    timer.interrupts = BIT(1) | BIT(2);
    on_timer_compare1 = count_cc1_callback;
    esb_timer_handler(NRF_TIMER_EVENT_COMPARE2, NULL);
    CHECK(cc1_callbacks == 0U && timer.shorts == cc1_shorts);
    CHECK((timer.interrupts & BIT(1)) != 0U);
    esb_timer_handler(NRF_TIMER_EVENT_COMPARE1, NULL);
    CHECK(cc1_callbacks == 1U && timer.shorts == cc1_shorts);
    /* Existing CC1 no-ACK completion still reports exactly one success. */
    on_timer_compare1 = on_radio_end_tx_noack;
    esb_timer_handler(NRF_TIMER_EVENT_COMPARE1, NULL);
    CHECK(esb_state == ESB_STATE_PTX_TXIDLE && tx_fifo.count == 0U);
    CHECK(interrupt_flags == INT_TX_SUCCESS_MSK && completion_irqs == 1U);
    CHECK((timer.interrupts & BIT(1)) == 0U);
}
static void noack_completion(void) {
    reset(); first_ramp(false);
    esb_state = ESB_STATE_PTX_TX;
    on_radio_disabled = on_radio_disabled_tx_noack;
    pending_radio_disabled = true; service_radio();
    CHECK(!timer.running && timer.counter == 0U);
    CHECK(esb_state == ESB_STATE_IDLE && tx_fifo.count == 0U);
    CHECK(interrupt_flags == INT_TX_SUCCESS_MSK && completion_irqs == 1U);
    CHECK(removed_payloads == 1U && radio_retry_launches == 0U);
}
int main(int argc, char **argv) {
    struct { const char *name; void (*run)(void); } cases[] = {
        {"delayed_radio_irq", delayed_radio_irq}, {"elapsed_ack_deadline", elapsed_ack_deadline},
        {"healthy_ack_address", healthy_ack_address}, {"cc1_preservation", cc1_preservation},
        {"noack_completion", noack_completion},
    };
    unsigned int ran = 0;
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc == 1 || strcmp(argv[1], cases[i].name) == 0) { cases[i].run(); ran++; }
    }
    CHECK(ran == (argc == 1 ? 5U : 1U));
    printf("%u SDK PTX timer runtime cases passed\n", ran);
    return 0;
}
