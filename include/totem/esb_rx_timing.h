/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

/* An input timer (hold-tap, combo, or sticky/tap-dance behavior) must not
 * overtake an already-received ESB event at or
 * before its deadline. This only observes local FIFO metadata, never RF data
 * or unauthenticated key positions, and does not drain events synchronously. */
#if IS_ENABLED(CONFIG_TOTEM_ESB_COMPAT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
bool totem_esb_rx_pending_before(int64_t deadline);
#else
static inline bool totem_esb_rx_pending_before(int64_t deadline) {
    (void)deadline;
    return false;
}
#endif
