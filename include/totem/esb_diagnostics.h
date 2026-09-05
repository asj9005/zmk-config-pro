/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once

enum totem_esb_diag_stage {
    TOTEM_DIAG_CRYPTO_INIT,
    TOTEM_DIAG_CRYPTO_KAT,
    TOTEM_DIAG_CRYPTO_ROOTS,
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
    TOTEM_DIAG_EVENT_COUNT,
};

#if defined(CONFIG_TOTEM_ESB_DIAGNOSTICS)

/* Initialization context only: records a result and prints one status line. */
void totem_esb_diag_stage(enum totem_esb_diag_stage stage, int result);

/* ISR-safe counters only. FRAME_ERR also records the most recent error code. */
void totem_esb_diag_event(enum totem_esb_diag_event event, int value);

/* Preserve the last KAT checkpoint even when early console output is dropped. */
void totem_esb_diag_kat(int checkpoint, int status);

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

#endif
