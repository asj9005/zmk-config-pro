/* SPDX-License-Identifier: MIT */
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Portable PRX software queue. The caller serializes all access. */
#define TOTEM_ESB_PRX_NONE UINT16_MAX
#define TOTEM_ESB_PRX_PIPE_LIMIT \
    ((CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS + CONFIG_ESB_PIPE_COUNT - 1) / \
     CONFIG_ESB_PIPE_COUNT)

struct totem_esb_prx_packet {
    uint8_t data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    uint16_t msg_id;
    uint16_t length;
    uint8_t pipe;
    uint32_t enqueued_at;
};

struct totem_esb_prx_queue {
    struct {
        struct totem_esb_prx_packet packet;
        uint16_t next;
        bool used;
    } entries[CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS];
    uint16_t head[CONFIG_ESB_PIPE_COUNT];
    uint16_t tail[CONFIG_ESB_PIPE_COUNT];
    uint16_t count[CONFIG_ESB_PIPE_COUNT];
    uint8_t next_pipe;
};

static inline bool totem_esb_prx_same(const struct totem_esb_prx_packet *a,
                                      const struct totem_esb_prx_packet *b) {
    /* v2 commands have no wire sequence: equal bytes with a new transport ID
     * can be an intentional second behavior invocation (including A/B/A).
     */
    return a->pipe == b->pipe && a->msg_id == b->msg_id && a->length == b->length &&
           memcmp(a->data, b->data, a->length) == 0;
}

static inline bool totem_esb_prx_expired(uint32_t now, uint32_t enqueued_at,
                                         uint32_t ttl) {
    return (uint32_t)(now - enqueued_at) >= ttl;
}

static inline void totem_esb_prx_queue_init(struct totem_esb_prx_queue *q) {
    memset(q, 0, sizeof(*q));
    for (unsigned int pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        q->head[pipe] = TOTEM_ESB_PRX_NONE;
        q->tail[pipe] = TOTEM_ESB_PRX_NONE;
    }
}

static inline const struct totem_esb_prx_packet *
totem_esb_prx_peek(const struct totem_esb_prx_queue *q, uint8_t pipe) {
    return q->head[pipe] == TOTEM_ESB_PRX_NONE ? NULL :
           &q->entries[q->head[pipe]].packet;
}

static inline void totem_esb_prx_pop(struct totem_esb_prx_queue *q, uint8_t pipe) {
    uint16_t index = q->head[pipe];
    if (index == TOTEM_ESB_PRX_NONE) {
        return;
    }
    q->head[pipe] = q->entries[index].next;
    q->entries[index].used = false;
    q->count[pipe]--;
    if (q->head[pipe] == TOTEM_ESB_PRX_NONE) {
        q->tail[pipe] = TOTEM_ESB_PRX_NONE;
    }
}

static inline void totem_esb_prx_clear_pipe(struct totem_esb_prx_queue *q,
                                            uint8_t pipe) {
    while (totem_esb_prx_peek(q, pipe) != NULL) {
        totem_esb_prx_pop(q, pipe);
    }
}

/* 0: queued, 1: exact pending duplicate (original expiry retained). */
static inline int totem_esb_prx_offer(struct totem_esb_prx_queue *q,
                                      const struct totem_esb_prx_packet *packet) {
    if (packet->pipe >= CONFIG_ESB_PIPE_COUNT || packet->length == 0 ||
        packet->length > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -EINVAL;
    }
    uint8_t pipe = packet->pipe;
    for (uint16_t i = q->head[pipe]; i != TOTEM_ESB_PRX_NONE;
         i = q->entries[i].next) {
        if (totem_esb_prx_same(&q->entries[i].packet, packet)) {
            return 1;
        }
    }
    /* A disconnected peer cannot take every software queue slot. */
    if (q->count[pipe] >= TOTEM_ESB_PRX_PIPE_LIMIT) {
        return -ENOSPC;
    }
    for (uint16_t i = 0; i < CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS; i++) {
        if (q->entries[i].used) {
            continue;
        }
        q->entries[i].packet = *packet;
        q->entries[i].next = TOTEM_ESB_PRX_NONE;
        q->entries[i].used = true;
        if (q->tail[pipe] == TOTEM_ESB_PRX_NONE) {
            q->head[pipe] = i;
        } else {
            q->entries[q->tail[pipe]].next = i;
        }
        q->tail[pipe] = i;
        q->count[pipe]++;
        return 0;
    }
    return -ENOSPC;
}

/* Each pipe is FIFO; ready pipes rotate independently of blocked peers. */
static inline int totem_esb_prx_next(struct totem_esb_prx_queue *q,
                                     uint32_t blocked_pipes) {
    for (unsigned int n = 0; n < CONFIG_ESB_PIPE_COUNT; n++) {
        uint8_t pipe = (q->next_pipe + n) % CONFIG_ESB_PIPE_COUNT;
        if (!(blocked_pipes & (UINT32_C(1) << pipe)) &&
            q->head[pipe] != TOTEM_ESB_PRX_NONE) {
            q->next_pipe = (pipe + 1) % CONFIG_ESB_PIPE_COUNT;
            return pipe;
        }
    }
    return -1;
}

static inline unsigned int totem_esb_prx_expire(struct totem_esb_prx_queue *q,
                                                uint32_t now, uint32_t ttl) {
    unsigned int dropped = 0;
    for (uint8_t pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        const struct totem_esb_prx_packet *packet;
        while ((packet = totem_esb_prx_peek(q, pipe)) != NULL &&
               totem_esb_prx_expired(now, packet->enqueued_at, ttl)) {
            totem_esb_prx_pop(q, pipe);
            dropped++;
        }
    }
    return dropped;
}
