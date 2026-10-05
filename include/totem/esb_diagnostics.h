/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>

enum totem_esb_diag_stage {
    TOTEM_DIAG_CRYPTO_INIT,
    TOTEM_DIAG_CRYPTO_KAT,
    TOTEM_DIAG_CRYPTO_ROOTS,
    TOTEM_DIAG_BOOT_NONCE,
    TOTEM_DIAG_CLOCK,
    TOTEM_DIAG_RADIO,
    TOTEM_DIAG_TRANSPORT,
    TOTEM_DIAG_STAGE_COUNT,
};

enum totem_esb_diag_event {
    TOTEM_DIAG_TX_OK,
    TOTEM_DIAG_TX_FAIL,
    TOTEM_DIAG_RX,
    TOTEM_DIAG_FRAME_OK,
    TOTEM_DIAG_FRAME_ERR,
    TOTEM_DIAG_HELLO,
    TOTEM_DIAG_CHALLENGE,
    TOTEM_DIAG_READY,
    TOTEM_DIAG_SESSION_OK,
    TOTEM_DIAG_RX_OVERFLOW,
    TOTEM_DIAG_RX_INVALID_POSITION,
    TOTEM_DIAG_HOLD_TAP_OVERFLOW,
    TOTEM_DIAG_SCAN_OVERFLOW,
    TOTEM_DIAG_SCAN_RESYNC,
    TOTEM_DIAG_USB_RETRY,
    TOTEM_DIAG_USB_OVERFLOW,
    TOTEM_DIAG_INPUT_RETRY,
    TOTEM_DIAG_INPUT_OVERFLOW,
    TOTEM_DIAG_EVENT_COUNT,
};

enum totem_esb_diag_tx_step {
    TOTEM_DIAG_TX_SEND,
    TOTEM_DIAG_TX_WRITE,
    TOTEM_DIAG_TX_START,
    TOTEM_DIAG_HF_REQUEST,
    TOTEM_DIAG_HF_CALLBACK,
    TOTEM_DIAG_HF_WAIT,
    TOTEM_DIAG_RADIO_BUSY,
    TOTEM_DIAG_TX_STEP_COUNT,
};

#if defined(CONFIG_TOTEM_ESB_DIAGNOSTICS)

/* Initialization context only: records a result and prints one status line. */
void totem_esb_diag_stage(enum totem_esb_diag_stage stage, int result);

/* ISR-safe counters only. FRAME_ERR also records the most recent error code. */
void totem_esb_diag_event(enum totem_esb_diag_event event, int value);

/* Preserve the last KAT checkpoint even when early console output is dropped. */
void totem_esb_diag_kat(int checkpoint, int status);

/* ISR-safe: EINPROGRESS marks entry; other results count completed calls. */
void totem_esb_diag_tx_step(enum totem_esb_diag_tx_step step, int result);

/* ISR-safe high-water observations; no key content or per-key timestamps. */
void totem_esb_diag_rx_observe(uint32_t queued_bytes, uint32_t age_ms);
void totem_esb_diag_scan_observe(uint32_t queued_events);
void totem_esb_diag_usb_observe(uint32_t queued_reports);

#else

static inline void totem_esb_diag_stage(enum totem_esb_diag_stage stage, int result) {
    (void)stage;
    (void)result;
}

static inline void totem_esb_diag_event(enum totem_esb_diag_event event, int value) {
    (void)event;
    (void)value;
}

static inline void totem_esb_diag_kat(int checkpoint, int status) {
    (void)checkpoint;
    (void)status;
}

static inline void totem_esb_diag_tx_step(enum totem_esb_diag_tx_step step, int result) {
    (void)step;
    (void)result;
}

static inline void totem_esb_diag_rx_observe(uint32_t queued_bytes, uint32_t age_ms) {
    (void)queued_bytes;
    (void)age_ms;
}
static inline void totem_esb_diag_scan_observe(uint32_t queued_events) { (void)queued_events; }
static inline void totem_esb_diag_usb_observe(uint32_t queued_reports) { (void)queued_reports; }

#endif
