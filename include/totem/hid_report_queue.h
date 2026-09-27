/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TOTEM_HID_QUEUE_SLOTS 64U
#define TOTEM_HID_REPORT_BYTES 64U
#define TOTEM_HID_REPORT_KINDS 3U

enum totem_hid_kind { TOTEM_HID_KEYBOARD, TOTEM_HID_CONSUMER, TOTEM_HID_MOUSE };

struct totem_hid_packet {
    uint8_t length;
    uint8_t data[TOTEM_HID_REPORT_BYTES];
};

/* All operations are serialized by the caller. Successful writes alone pop a
 * head. If finite storage overflows, keep the latest state for each report kind
 * behind already accepted transitions. This cannot recover a whole lost tap,
 * but a final release remains pending even if no further input ever arrives.
 */
struct totem_hid_queue {
    struct totem_hid_packet packets[TOTEM_HID_QUEUE_SLOTS];
    struct totem_hid_packet recovery[TOTEM_HID_REPORT_KINDS];
    uint16_t head;
    uint16_t count;
    uint8_t recovery_mask;
    uint32_t generation;
};

/* Invalidates every queued format/state without touching an in-flight DMA copy. */
static inline void totem_hid_queue_reset(struct totem_hid_queue *queue) {
    queue->head = 0;
    queue->count = 0;
    queue->recovery_mask = 0;
    queue->generation++;
}

static inline int totem_hid_queue_offer(struct totem_hid_queue *queue, enum totem_hid_kind kind,
                                      const uint8_t *report, const uint8_t *recovery,
                                      uint8_t length) {
    if ((unsigned int)kind >= TOTEM_HID_REPORT_KINDS || length == 0 ||
        length > TOTEM_HID_REPORT_BYTES) {
        return -1;
    }
    queue->recovery[kind].length = length;
    memcpy(queue->recovery[kind].data, recovery, length);
    if (queue->count == TOTEM_HID_QUEUE_SLOTS || queue->recovery_mask != 0) {
        queue->recovery_mask |= (uint8_t)(1U << kind);
        return 1;
    }
    uint16_t tail = (uint16_t)((queue->head + queue->count) % TOTEM_HID_QUEUE_SLOTS);
    queue->packets[tail].length = length;
    memcpy(queue->packets[tail].data, report, length);
    queue->count++;
    return 0;
}

static inline bool totem_hid_queue_peek(struct totem_hid_queue *queue,
                                      struct totem_hid_packet *packet) {
    if (queue->count == 0 && queue->recovery_mask != 0) {
        for (unsigned int kind = 0; kind < TOTEM_HID_REPORT_KINDS; kind++) {
            if ((queue->recovery_mask & (1U << kind)) == 0) {
                continue;
            }
            queue->packets[queue->head] = queue->recovery[kind];
            queue->recovery_mask &= (uint8_t)~(1U << kind);
            queue->count = 1;
            break;
        }
    }
    if (queue->count == 0) {
        return false;
    }
    *packet = queue->packets[queue->head];
    return true;
}

static inline void totem_hid_queue_pop(struct totem_hid_queue *queue) {
    if (queue->count != 0) {
        queue->head = (uint16_t)((queue->head + 1U) % TOTEM_HID_QUEUE_SLOTS);
        queue->count--;
    }
}

static inline bool totem_hid_queue_pending(const struct totem_hid_queue *queue) {
    return queue->count != 0 || queue->recovery_mask != 0;
}
