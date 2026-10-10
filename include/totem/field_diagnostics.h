/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOTEM_FIELD_SCOPE_LEFT 0U
#define TOTEM_FIELD_SCOPE_RIGHT 1U
#define TOTEM_FIELD_SCOPE_LOCAL 2U
#define TOTEM_FIELD_SCOPE_COUNT 3U

enum totem_field_metric {
    TOTEM_FIELD_RX_QUEUE,
    TOTEM_FIELD_RX_PROCESS,
    TOTEM_FIELD_HOLD_TAP_BASE_E,
    TOTEM_FIELD_HOLD_TAP_BASE_R,
    TOTEM_FIELD_HOLD_TAP_MOUSE_FAST,
    TOTEM_FIELD_HOLD_TAP_MOUSE_SLOW,
    TOTEM_FIELD_HOLD_TAP_OTHER,
    TOTEM_FIELD_TIMER_LATE,
    TOTEM_FIELD_USB_QUEUE,
    TOTEM_FIELD_USB_QUEUE_RECOVERY,
    TOTEM_FIELD_USB_QUEUE_RESYNC,
    TOTEM_FIELD_USB_TRANSFER,
    TOTEM_FIELD_METRIC_COUNT,
};

enum totem_field_reason {
    TOTEM_FIELD_PEER_TIMEOUT,
    TOTEM_FIELD_AUTH_RESTART,
    TOTEM_FIELD_SESSION_ESTABLISHED,
    TOTEM_FIELD_HANDSHAKE_TIMEOUT,
    TOTEM_FIELD_TX_QUEUE_FULL,
    TOTEM_FIELD_PRESESSION_OVERFLOW,
    TOTEM_FIELD_RX_OVERFLOW,
    TOTEM_FIELD_SCAN_OVERFLOW,
    TOTEM_FIELD_USB_OVERFLOW,
    TOTEM_FIELD_USB_RETRY,
    TOTEM_FIELD_INPUT_OVERFLOW,
    TOTEM_FIELD_HOLD_TAP_OVERFLOW,
    TOTEM_FIELD_USB_RESET,
    TOTEM_FIELD_USB_SUSPEND,
    TOTEM_FIELD_USB_RESUME,
    TOTEM_FIELD_USB_TIMING_DISCARD,
    TOTEM_FIELD_REASON_COUNT,
};

enum totem_field_group {
    TOTEM_FIELD_BASE_E,
    TOTEM_FIELD_BASE_R,
    TOTEM_FIELD_MOUSE_FAST,
    TOTEM_FIELD_MOUSE_SLOW,
    TOTEM_FIELD_OTHER,
    TOTEM_FIELD_GROUP_COUNT,
};

#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
/* Local uptime converted from kernel ticks. Never subtract clocks of different
 * devices. The snapshot advertises tick_hz; RX_QUEUE has 1 ms resolution. */
uint64_t totem_field_now_us(void);
/* ISR-safe, bounded RAM updates; no printing, allocation or transmission. */
void totem_field_observe(enum totem_field_metric metric, uint64_t duration_us);
/* scope is left/right/local. code is an errno or bounded diagnostic category,
 * never a key position/code, payload, nonce or wire session identifier. */
void totem_field_issue(enum totem_field_reason reason, uint8_t scope, int32_t code);
/* Aggregate only: no timestamp is retained for individual tap/hold decisions. */
void totem_field_hold_tap(enum totem_field_group group, bool hold);
#else
static inline uint64_t totem_field_now_us(void) { return 0; }
static inline void totem_field_observe(enum totem_field_metric metric, uint64_t duration_us) {
    (void)metric; (void)duration_us;
}
static inline void totem_field_issue(enum totem_field_reason reason, uint8_t scope, int32_t code) {
    (void)reason; (void)scope; (void)code;
}
static inline void totem_field_hold_tap(enum totem_field_group group, bool hold) {
    (void)group; (void)hold;
}
#endif
