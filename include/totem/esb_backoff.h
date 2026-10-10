/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

struct totem_esb_backoff {
    uint32_t next_ms;
};

static inline void totem_esb_backoff_reset(struct totem_esb_backoff *backoff,
                                          uint32_t initial_ms) {
    backoff->next_ms = initial_ms;
}

/* Consume a delay and double the next one without overflowing uint32_t. */
static inline uint32_t totem_esb_backoff_take(struct totem_esb_backoff *backoff,
                                            uint32_t maximum_ms) {
    uint32_t delay_ms = backoff->next_ms > maximum_ms ? maximum_ms : backoff->next_ms;
    backoff->next_ms = delay_ms > maximum_ms / 2U ? maximum_ms : delay_ms * 2U;
    return delay_ms;
}
