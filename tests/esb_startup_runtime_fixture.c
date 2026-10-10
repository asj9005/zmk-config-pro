/* SPDX-License-Identifier: MIT
 * Production callback bodies are inserted without rewriting their logic.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fail on stderr without opening the Windows CRT assertion dialog. */
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "startup assertion failed: %s:%d (%s): %s\n", __FILE__, __LINE__, __func__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define IS_ENABLED(option) (option)
#define CONFIG_TOTEM_ESB_V3 1
#define CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START 0
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 64
#define CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS 2
#define BIT(bit) (1U << (bit))
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(ms) (ms)
#define ESB_WIRE_EVENT_ZMK 0
#define ESB_WIRE_EVENT_KEY_STATE 1
#define ARG_UNUSED(value) ((void)(value))
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT 1
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT 2
#ifndef QUEUE_CAPACITY
#define QUEUE_CAPACITY 4U
#endif
#define TRANSMIT_CAPACITY 256U

enum esb_v3_peripheral_state {
    ESB_V3_NO_SESSION,
    ESB_V3_WAIT_CHALLENGE,
    ESB_V3_WAIT_SESSION_OK,
    ESB_V3_ESTABLISHED,
};

/* Only the members used by the production input callbacks are modeled. */
struct zmk_split_transport_peripheral_event {
    int type;
    union {
        struct { uint32_t position; bool pressed; } key_position_event;
        struct { uint8_t level; } battery_event;
    } data;
};
struct k_mutex { int unused; };
struct k_work { int unused; };
struct k_work_delayable { unsigned int scheduled; int delay; };
struct fake_msgq {
    struct zmk_split_transport_peripheral_event events[QUEUE_CAPACITY];
    unsigned int used;
};
typedef int atomic_t;

static struct k_mutex event_mutex;
static struct k_work_delayable handshake_work, flush_presession_work, key_state_work;
static struct fake_msgq presession_events;
static struct { uint8_t keys[8]; } local_key_state;
static struct { unsigned int resets; } handshake_backoff;
static atomic_t transport_ready;
static enum esb_v3_peripheral_state secure_state;
static unsigned int lock_depth, pressure_count, enqueue_calls, rekey_calls;
static unsigned int transmitted_count, snapshot_count;
static int enqueue_result, rekey_result;
static struct zmk_split_transport_peripheral_event transmitted[TRANSMIT_CAPACITY];
static uint8_t received_key_state[8];
static struct zmk_split_transport_peripheral_event last_attempt;

#define atomic_get(value) (*(value))
static enum esb_v3_peripheral_state get_secure_state(void) { return secure_state; }
static int k_mutex_lock(struct k_mutex *mutex, int timeout) {
    assert(mutex == &event_mutex && timeout == K_FOREVER);
    assert(lock_depth++ == 0U);
    return 0;
}
static int k_mutex_unlock(struct k_mutex *mutex) {
    assert(mutex == &event_mutex && lock_depth == 1U);
    lock_depth--;
    return 0;
}
static unsigned int k_msgq_num_used_get(struct fake_msgq *queue) {
    assert(queue == &presession_events && lock_depth == 1U);
    return queue->used;
}
static int k_msgq_put(struct fake_msgq *queue, const void *event, int timeout) {
    assert(queue == &presession_events && timeout == K_NO_WAIT && lock_depth == 1U);
    if (queue->used == QUEUE_CAPACITY) {
        return -ENOMSG;
    }
    queue->events[queue->used++] = *(const struct zmk_split_transport_peripheral_event *)event;
    return 0;
}
static int k_msgq_peek(struct fake_msgq *queue, void *event) {
    assert(queue == &presession_events && lock_depth == 1U);
    if (queue->used == 0U) {
        return -ENOMSG;
    }
    *(struct zmk_split_transport_peripheral_event *)event = queue->events[0];
    return 0;
}
static int k_msgq_get(struct fake_msgq *queue, void *event, int timeout) {
    assert(timeout == K_NO_WAIT);
    int result = k_msgq_peek(queue, event);
    if (result == 0) {
        queue->used--;
        memmove(&queue->events[0], &queue->events[1],
                queue->used * sizeof(queue->events[0]));
    }
    return result;
}
static int k_work_reschedule(struct k_work_delayable *work, int delay) {
    assert(lock_depth == 1U);
    assert(work == &handshake_work || work == &flush_presession_work || work == &key_state_work);
    work->scheduled++;
    work->delay = delay;
    return 1;
}
static void totem_esb_transport_queue_pressure(bool presession) {
    assert(presession && lock_depth == 1U);
    pressure_count++;
}
static void totem_esb_backoff_reset(void *backoff, unsigned int delay) {
    assert(backoff == &handshake_backoff && lock_depth == 1U);
    assert(delay == CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
    handshake_backoff.resets++;
}
static int enqueue_wire_event(int type, const struct zmk_split_transport_peripheral_event *event) {
    assert((type == ESB_WIRE_EVENT_ZMK || type == ESB_WIRE_EVENT_KEY_STATE) && lock_depth == 1U);
    enqueue_calls++;
    if (event != NULL) {
        last_attempt = *event;
    }
    if (enqueue_result != 0) {
        return enqueue_result;
    }
    if (type == ESB_WIRE_EVENT_KEY_STATE) {
        assert(event == NULL && presession_events.used == 0U);
        memcpy(received_key_state, local_key_state.keys, sizeof(received_key_state));
        snapshot_count++;
        return 0;
    }
    assert(event != NULL && transmitted_count < TRANSMIT_CAPACITY);
    transmitted[transmitted_count++] = *event;
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        uint32_t position = event->data.key_position_event.position;
        uint8_t mask = BIT(position % 8U);
        if (event->data.key_position_event.pressed) {
            received_key_state[position / 8U] |= mask;
        } else {
            received_key_state[position / 8U] &= (uint8_t)~mask;
        }
    }
    return 0;
}
static int enqueue_v3_frame(int type, const struct zmk_split_transport_peripheral_event *event) {
    return enqueue_wire_event(type, event);
}
static int begin_fresh_v3_handshake(void) {
    assert(lock_depth == 1U);
    rekey_calls++;
    if (rekey_result == 0) {
        secure_state = ESB_V3_WAIT_CHALLENGE;
    }
    return rekey_result;
}

/* ACTUAL_FIRMWARE_FUNCTIONS */

static void reset(void) {
    memset(&presession_events, 0, sizeof(presession_events));
    memset(&local_key_state, 0, sizeof(local_key_state));
    memset(&handshake_backoff, 0, sizeof(handshake_backoff));
    memset(&handshake_work, 0, sizeof(handshake_work));
    memset(&flush_presession_work, 0, sizeof(flush_presession_work));
    memset(&key_state_work, 0, sizeof(key_state_work));
    memset(transmitted, 0, sizeof(transmitted));
    memset(received_key_state, 0, sizeof(received_key_state));
    memset(&last_attempt, 0, sizeof(last_attempt));
    transport_ready = 0;
    secure_state = ESB_V3_NO_SESSION;
    lock_depth = pressure_count = enqueue_calls = rekey_calls = 0U;
    transmitted_count = snapshot_count = 0U;
    enqueue_result = rekey_result = 0;
}
static struct zmk_split_transport_peripheral_event key(uint32_t position, bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };
    return event;
}
static struct zmk_split_transport_peripheral_event battery(uint8_t level) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
        .data.battery_event = {.level = level},
    };
    return event;
}
static int report(struct zmk_split_transport_peripheral_event event) {
    int result = split_peripheral_esb_report_event(&event);
    assert(lock_depth == 0U);
    return result;
}
static void expect_key(unsigned int index, uint32_t position, bool pressed) {
    assert(index < presession_events.used);
    const struct zmk_split_transport_peripheral_event *event = &presession_events.events[index];
    assert(event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT);
    assert(event->data.key_position_event.position == position);
    assert(event->data.key_position_event.pressed == pressed);
}
static bool held(uint32_t position) {
    return (local_key_state.keys[position / 8U] & BIT(position % 8U)) != 0;
}
static void expect_no_work(void) {
    assert(handshake_work.scheduled == 0U && flush_presession_work.scheduled == 0U);
    assert(key_state_work.scheduled == 0U && handshake_backoff.resets == 0U);
    assert(enqueue_calls == 0U && rekey_calls == 0U);
}

static void startup_preserves_press_release_order(void) {
    reset();
    assert(report(key(5U, true)) == 0 && held(5U));
    assert(report(key(9U, true)) == 0 && held(9U));
    assert(report(key(5U, false)) == 0 && !held(5U));
    assert(report(key(9U, false)) == 0 && !held(9U));
    assert(presession_events.used == 4U);
    expect_key(0U, 5U, true);
    expect_key(1U, 9U, true);
    expect_key(2U, 5U, false);
    expect_key(3U, 9U, false);
    expect_no_work();
}
static void nonce_ready_still_waits_for_radio_startup(void) {
    reset();
    secure_state = ESB_V3_WAIT_CHALLENGE;
    assert(report(key(7U, true)) == 0);
    assert(report(battery(73U)) == 0);
    assert(report(key(7U, false)) == 0);
    expect_key(0U, 7U, true);
    assert(presession_events.events[1].type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT);
    assert(presession_events.events[1].data.battery_event.level == 73U);
    expect_key(2U, 7U, false);
    expect_no_work();
}
static void full_startup_queue_keeps_current_bitmap(void) {
    reset();
    assert(report(key(5U, true)) == 0);
    for (unsigned int i = 1U; i < QUEUE_CAPACITY; i++) {
        assert(report(battery((uint8_t)i)) == 0);
    }
    assert(report(key(5U, false)) == -ENOSPC && !held(5U));
    assert(report(key(9U, true)) == -ENOSPC && held(9U));
    assert(presession_events.used == QUEUE_CAPACITY && pressure_count == 2U);
    expect_key(0U, 5U, true);
    for (unsigned int i = 1U; i < QUEUE_CAPACITY; i++) {
        assert(presession_events.events[i].data.battery_event.level == i);
    }
    expect_no_work();
}
static void ready_input_wakes_handshake(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_WAIT_CHALLENGE;
    assert(report(key(5U, true)) == 0);
    expect_key(0U, 5U, true);
    assert(handshake_work.scheduled == 1U && handshake_work.delay == K_NO_WAIT);
    assert(handshake_backoff.resets == 1U && enqueue_calls == 0U);
    assert(flush_presession_work.scheduled == 0U && key_state_work.scheduled == 0U);
}
static void established_input_uses_normal_sender(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    assert(report(key(5U, true)) == 0 && held(5U));
    assert(report(key(5U, false)) == 0 && !held(5U));
    assert(presession_events.used == 0U && enqueue_calls == 2U);
    assert(transmitted[0].data.key_position_event.pressed);
    assert(!transmitted[1].data.key_position_event.pressed);
    assert(handshake_work.scheduled == 0U && handshake_backoff.resets == 0U);
    assert(flush_presession_work.scheduled == 0U && key_state_work.scheduled == 0U);
}
static void newly_ready_input_cannot_overtake_startup_edges(void) {
    reset();
    assert(report(key(5U, true)) == 0);
    assert(report(key(5U, false)) == 0);
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    assert(report(key(9U, true)) == 0);
    assert(enqueue_calls == 0U && presession_events.used == 3U);
    expect_key(0U, 5U, true);
    expect_key(1U, 5U, false);
    expect_key(2U, 9U, true);
    assert(flush_presession_work.scheduled == 1U);
    assert(handshake_work.scheduled == 0U && key_state_work.scheduled == 0U);
}
static void flush(void) {
    flush_presession_work_cb(NULL);
    assert(lock_depth == 0U);
}
static void snapshot(void) {
    key_state_work_cb(NULL);
    assert(lock_depth == 0U);
}
static void expect_transmitted_key(unsigned int index, uint32_t position, bool pressed) {
    assert(index < transmitted_count);
    const struct zmk_split_transport_peripheral_event *event = &transmitted[index];
    assert(event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT);
    assert(event->data.key_position_event.position == position);
    assert(event->data.key_position_event.pressed == pressed);
}
static void ready_tx_pressure_retains_complete_tap(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = -ENOSPC;
    assert(report(key(5U, true)) == 0 && held(5U));
    assert(report(key(5U, false)) == 0 && !held(5U));
    assert(report(key(9U, true)) == 0 && held(9U));
    assert(enqueue_calls == 1U && transmitted_count == 0U);
    assert(presession_events.used == 3U);
    assert(handshake_work.scheduled == 0U && flush_presession_work.scheduled == 3U);
    assert(flush_presession_work.delay == K_NO_WAIT && key_state_work.scheduled == 0U);

    /* The failed flush leaves the same head for retry. New input stays after it. */
    flush();
    assert(enqueue_calls == 2U && presession_events.used == 3U);
    assert(last_attempt.data.key_position_event.position == 5U);
    assert(last_attempt.data.key_position_event.pressed);
    assert(flush_presession_work.delay == K_MSEC(1));
    expect_key(0U, 5U, true);
    assert(report(key(9U, false)) == 0 && !held(9U));
    expect_key(1U, 5U, false);
    expect_key(2U, 9U, true);
    expect_key(3U, 9U, false);

    enqueue_result = 0;
    flush();
    assert(presession_events.used == 0U && transmitted_count == 4U);
    expect_transmitted_key(0U, 5U, true);
    expect_transmitted_key(1U, 5U, false);
    expect_transmitted_key(2U, 9U, true);
    expect_transmitted_key(3U, 9U, false);
    assert(key_state_work.scheduled == 1U && key_state_work.delay == K_NO_WAIT);
    snapshot();
    assert(snapshot_count == 1U && received_key_state[0] == 0U && received_key_state[1] == 0U);
}
static void pressure_queue_overflow_repairs_release_after_drain(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    assert(report(key(5U, true)) == 0);
    assert(received_key_state[0] == BIT(5));
    enqueue_result = -ENOSPC;
    for (unsigned int i = 0U; i < QUEUE_CAPACITY; i++) {
        assert(report(battery((uint8_t)i)) == 0);
    }
    assert(report(key(5U, false)) == -ENOSPC && !held(5U));
    assert(pressure_count == 1U && presession_events.used == QUEUE_CAPACITY);
    unsigned int before = enqueue_calls;
    snapshot();
    assert(enqueue_calls == before && snapshot_count == 0U);
    assert(flush_presession_work.delay == K_NO_WAIT);

    enqueue_result = 0;
    while (presession_events.used != 0U) {
        unsigned int old_used = presession_events.used;
        flush();
        assert(old_used - presession_events.used <= ESB_V3_PRESESSION_FLUSH_BATCH);
        assert(old_used > presession_events.used);
        assert(snapshot_count == 0U);
    }
    assert(transmitted_count == 1U + QUEUE_CAPACITY);
    for (unsigned int i = 0U; i < QUEUE_CAPACITY; i++) {
        assert(transmitted[1U + i].type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT);
        assert(transmitted[1U + i].data.battery_event.level == i);
    }
    assert(received_key_state[0] == BIT(5));
    snapshot();
    assert(snapshot_count == 1U && received_key_state[0] == 0U);
}
static void pressure_queue_batches_preserve_newer_input_order(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = -ENOSPC;
    for (unsigned int i = 0U; i < QUEUE_CAPACITY; i++) {
        assert(report(key(5U, (i & 1U) == 0U)) == 0);
    }
    enqueue_result = 0;
    flush();
    assert(transmitted_count == ESB_V3_PRESESSION_FLUSH_BATCH);
    if (QUEUE_CAPACITY > ESB_V3_PRESESSION_FLUSH_BATCH) {
        unsigned int before = enqueue_calls;
        assert(report(key(9U, true)) == 0);
        assert(enqueue_calls == before);
        assert(presession_events.used == QUEUE_CAPACITY - ESB_V3_PRESESSION_FLUSH_BATCH + 1U);
        assert(key_state_work.scheduled == 0U);
    } else {
        assert(report(key(9U, true)) == 0);
    }
    while (presession_events.used != 0U) {
        flush();
    }
    for (unsigned int i = 0U; i < QUEUE_CAPACITY; i++) {
        expect_transmitted_key(i, 5U, (i & 1U) == 0U);
    }
    expect_transmitted_key(QUEUE_CAPACITY, 9U, true);
    assert(transmitted_count == QUEUE_CAPACITY + 1U);
}
static void queued_pressure_waits_for_rekey_without_losing_head(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = -ENOSPC;
    assert(report(key(5U, true)) == 0);
    assert(report(key(5U, false)) == 0);
    enqueue_result = -EOVERFLOW;
    flush();
    assert(rekey_calls == 1U && secure_state == ESB_V3_WAIT_CHALLENGE);
    assert(presession_events.used == 2U && transmitted_count == 0U);
    unsigned int before = enqueue_calls;
    flush();
    assert(enqueue_calls == before);
    assert(report(key(9U, true)) == 0 && presession_events.used == 3U);
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = 0;
    flush();
    assert(presession_events.used == 0U && transmitted_count == 3U);
    expect_transmitted_key(0U, 5U, true);
    expect_transmitted_key(1U, 5U, false);
    expect_transmitted_key(2U, 9U, true);
}
static void ready_rekey_preserves_triggering_input(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = -EOVERFLOW;
    assert(report(key(5U, true)) == 0 && held(5U));
    assert(enqueue_calls == 1U && rekey_calls == 1U);
    expect_key(0U, 5U, true);
    assert(handshake_work.scheduled == 1U && key_state_work.scheduled == 0U);
}

int main(void) {
    startup_preserves_press_release_order();
    nonce_ready_still_waits_for_radio_startup();
    full_startup_queue_keeps_current_bitmap();
    ready_input_wakes_handshake();
    established_input_uses_normal_sender();
    newly_ready_input_cannot_overtake_startup_edges();
    ready_tx_pressure_retains_complete_tap();
    pressure_queue_overflow_repairs_release_after_drain();
    pressure_queue_batches_preserve_newer_input_order();
    queued_pressure_waits_for_rekey_without_losing_head();
    ready_rekey_preserves_triggering_input();
    puts("11 peripheral startup and backpressure input cases passed");
    return 0;
}
