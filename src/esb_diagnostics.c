/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#include <totem/esb_diagnostics.h>

#if defined(CONFIG_TOTEM_ESB_DIAGNOSTICS)

#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <esb.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/* Static initialization preserves results recorded before our SYS_INIT runs. */
static atomic_t stage_results[TOTEM_DIAG_STAGE_COUNT] = {
    [TOTEM_DIAG_CRYPTO_INIT] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_CRYPTO_KAT] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_CRYPTO_ROOTS] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_BOOT_NONCE] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_CLOCK] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_RADIO] = ATOMIC_INIT(INT_MIN),
    [TOTEM_DIAG_TRANSPORT] = ATOMIC_INIT(INT_MIN),
};
static atomic_t event_counts[TOTEM_DIAG_EVENT_COUNT];
static atomic_t last_frame_error;
static atomic_t last_kat_checkpoint;
static atomic_t last_kat_status;
static atomic_t tx_step_counts[TOTEM_DIAG_TX_STEP_COUNT];
static atomic_t tx_step_results[TOTEM_DIAG_TX_STEP_COUNT];

static const char *const stage_names[TOTEM_DIAG_STAGE_COUNT] = {
    [TOTEM_DIAG_CRYPTO_INIT] = "crypto_init",
    [TOTEM_DIAG_CRYPTO_KAT] = "crypto_kat",
    [TOTEM_DIAG_CRYPTO_ROOTS] = "crypto_roots",
    [TOTEM_DIAG_BOOT_NONCE] = "boot_nonce",
    [TOTEM_DIAG_CLOCK] = "clock",
    [TOTEM_DIAG_RADIO] = "radio",
    [TOTEM_DIAG_TRANSPORT] = "transport",
};

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define TOTEM_DIAG_ROLE "dongle"
#elif CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID == 1
#define TOTEM_DIAG_ROLE "left"
#elif CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID == 2
#define TOTEM_DIAG_ROLE "right"
#else
#define TOTEM_DIAG_ROLE "peripheral"
#endif

void totem_esb_diag_stage(enum totem_esb_diag_stage stage, int result) {
    if ((unsigned int)stage >= TOTEM_DIAG_STAGE_COUNT) {
        return;
    }

    atomic_set(&stage_results[stage], result);
    printk("[esb-diag] role=%s stage=%s result=%d\n", TOTEM_DIAG_ROLE,
           stage_names[stage], result);
#if defined(CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START)
    /* This profile invokes startup on its own thread, after USB DTR. Let the
     * USB workqueues deliver this checkpoint before the next operation. */
    k_msleep(200);
#endif
}

void totem_esb_diag_event(enum totem_esb_diag_event event, int value) {
    if ((unsigned int)event >= TOTEM_DIAG_EVENT_COUNT) {
        return;
    }

    if (event == TOTEM_DIAG_FRAME_ERR) {
        atomic_set(&last_frame_error, value);
    }
    atomic_inc(&event_counts[event]);
}

void totem_esb_diag_kat(int checkpoint, int status) {
    atomic_set(&last_kat_status, status);
    atomic_set(&last_kat_checkpoint, checkpoint);
}

void totem_esb_diag_tx_step(enum totem_esb_diag_tx_step step, int result) {
    if ((unsigned int)step >= TOTEM_DIAG_TX_STEP_COUNT) {
        return;
    }
    atomic_set(&tx_step_results[step], result);
    if (result != -EINPROGRESS) {
        atomic_inc(&tx_step_counts[step]);
    }
}

static void diagnostics_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(diagnostics_work, diagnostics_work_handler);

static void diagnostics_work_handler(struct k_work *work) {
    (void)work;
    /* Each field is an atomic observation; IRQs can advance counters between
     * fields. These are diagnostic snapshots, not an event-ordering trace. */
    char results[TOTEM_DIAG_STAGE_COUNT][12];
    unsigned long counts[TOTEM_DIAG_EVENT_COUNT];
    unsigned long tx_counts[TOTEM_DIAG_TX_STEP_COUNT];
    int tx_results[TOTEM_DIAG_TX_STEP_COUNT];

    for (unsigned int i = 0; i < TOTEM_DIAG_STAGE_COUNT; i++) {
        int result = (int)atomic_get(&stage_results[i]);
        if (result == INT_MIN) {
            snprintk(results[i], sizeof(results[i]), "na");
        } else {
            snprintk(results[i], sizeof(results[i]), "%d", result);
        }
    }
    for (unsigned int i = 0; i < TOTEM_DIAG_EVENT_COUNT; i++) {
        counts[i] = (unsigned long)(uint32_t)atomic_get(&event_counts[i]);
    }
    int frame_error = (int)atomic_get(&last_frame_error);
    for (unsigned int i = 0; i < TOTEM_DIAG_TX_STEP_COUNT; i++) {
        tx_counts[i] = (unsigned long)(uint32_t)atomic_get(&tx_step_counts[i]);
        tx_results[i] = (int)atomic_get(&tx_step_results[i]);
    }
    unsigned long uptime_ms = (unsigned long)k_uptime_get_32();

    printk("[esb-diag] role=%s uptime_ms=%lu crypto_init=%s crypto_kat=%s "
           "crypto_roots=%s boot_nonce=%s clock=%s radio=%s transport=%s kat_step=%d kat_status=%d\n",
           TOTEM_DIAG_ROLE, uptime_ms, results[TOTEM_DIAG_CRYPTO_INIT],
           results[TOTEM_DIAG_CRYPTO_KAT], results[TOTEM_DIAG_CRYPTO_ROOTS],
           results[TOTEM_DIAG_BOOT_NONCE],
           results[TOTEM_DIAG_CLOCK], results[TOTEM_DIAG_RADIO],
           results[TOTEM_DIAG_TRANSPORT], (int)atomic_get(&last_kat_checkpoint),
           (int)atomic_get(&last_kat_status));
    printk("[esb-diag] role=%s uptime_ms=%lu tx_ok=%lu tx_fail=%lu rx=%lu "
           "frame_ok=%lu frame_err=%lu last_frame_err=%d hello=%lu challenge=%lu "
           "ready=%lu session_ok=%lu\n",
           TOTEM_DIAG_ROLE, uptime_ms, counts[TOTEM_DIAG_TX_OK],
           counts[TOTEM_DIAG_TX_FAIL], counts[TOTEM_DIAG_RX],
           counts[TOTEM_DIAG_FRAME_OK], counts[TOTEM_DIAG_FRAME_ERR], frame_error,
           counts[TOTEM_DIAG_HELLO], counts[TOTEM_DIAG_CHALLENGE],
           counts[TOTEM_DIAG_READY], counts[TOTEM_DIAG_SESSION_OK]);
    /* step=completed_calls:last_result; 0:0 means not yet observed. A last
     * EINPROGRESS can identify entry without a recorded return. */
    printk("[esb-diag] role=%s uptime_ms=%lu tx_steps=done:result "
           "send=%lu:%d write=%lu:%d start=%lu:%d hf_request=%lu:%d "
           "hf_callback=%lu:%d hf_wait=%lu:%d radio_busy=%lu:%d\n",
           TOTEM_DIAG_ROLE, uptime_ms,
           tx_counts[TOTEM_DIAG_TX_SEND], tx_results[TOTEM_DIAG_TX_SEND],
           tx_counts[TOTEM_DIAG_TX_WRITE], tx_results[TOTEM_DIAG_TX_WRITE],
           tx_counts[TOTEM_DIAG_TX_START], tx_results[TOTEM_DIAG_TX_START],
           tx_counts[TOTEM_DIAG_HF_REQUEST], tx_results[TOTEM_DIAG_HF_REQUEST],
           tx_counts[TOTEM_DIAG_HF_CALLBACK], tx_results[TOTEM_DIAG_HF_CALLBACK],
           tx_counts[TOTEM_DIAG_HF_WAIT], tx_results[TOTEM_DIAG_HF_WAIT],
           tx_counts[TOTEM_DIAG_RADIO_BUSY], tx_results[TOTEM_DIAG_RADIO_BUSY]);

    struct esb_diagnostics radio;
    if (esb_get_diagnostics(&radio) == 0) {
        printk("[esb-diag] role=%s uptime_ms=%lu sdk_state=%lu radio_state=%lu "
               "tx_queued=%lu retries=%lu irq_flags=%lu radio_events=%lu "
               "timer_events=%lu timer_shorts=%lu radio_irq=%lu timer_irq=%lu\n",
               TOTEM_DIAG_ROLE, uptime_ms,
               (unsigned long)radio.state, (unsigned long)radio.radio_state,
               (unsigned long)radio.tx_queued, (unsigned long)radio.retries,
               (unsigned long)radio.irq_flags, (unsigned long)radio.radio_events,
               (unsigned long)radio.timer_events, (unsigned long)radio.timer_shorts,
               (unsigned long)radio.radio_irq, (unsigned long)radio.timer_irq);
    }

    k_work_schedule(&diagnostics_work, K_SECONDS(5));
}

static int diagnostics_init(void) {
    k_work_schedule(&diagnostics_work, K_SECONDS(5));
    return 0;
}

SYS_INIT(diagnostics_init, APPLICATION, 99);

#endif
