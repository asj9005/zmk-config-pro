/* Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Used by the firmware receiver and the native C fault-recovery tests. */
static inline bool totem_esb_key_state_get(const uint8_t *keys, size_t position) {
    return (keys[position / 8U] & (1U << (position % 8U))) != 0;
}

static inline void totem_esb_key_state_set(uint8_t *keys, size_t position, bool pressed) {
    uint8_t mask = (uint8_t)(1U << (position % 8U));
    if (pressed) {
        keys[position / 8U] |= mask;
    } else {
        keys[position / 8U] &= (uint8_t)~mask;
    }
}

static inline bool totem_esb_key_state_valid(const uint8_t *keys, size_t key_count) {
    if (keys == NULL || key_count == 0 || key_count > 256U) {
        return false;
    }
    size_t tail = key_count % 8U;
    return tail == 0 || (keys[key_count / 8U] & (uint8_t)~((1U << tail) - 1U)) == 0;
}

typedef void (*totem_esb_key_state_emit_t)(void *context, uint8_t position, bool pressed);

/* Release stale keys before restoring held keys. A repeated snapshot is inert.
 * Call only after the transport has checked source, session and sequence.
 */
static inline size_t totem_esb_key_state_reconcile(
    uint8_t *current, const uint8_t *desired, size_t key_count,
    totem_esb_key_state_emit_t emit, void *context) {
    if (current == NULL || emit == NULL || !totem_esb_key_state_valid(desired, key_count)) {
        return 0;
    }
    size_t changes = 0;
    for (unsigned int pass = 0; pass < 2; pass++) {
        bool pressed = pass != 0;
        for (size_t position = 0; position < key_count; position++) {
            if (totem_esb_key_state_get(desired, position) != pressed ||
                totem_esb_key_state_get(current, position) == pressed) {
                continue;
            }
            totem_esb_key_state_set(current, position, pressed);
            emit(context, (uint8_t)position, pressed);
            changes++;
        }
    }
    return changes;
}
