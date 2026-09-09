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
        fprintf(stderr, "startup assertion failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
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
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT 1
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT 2
#define QUEUE_CAPACITY 4U

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
static int enqueue_result, rekey_result;
static struct zmk_split_transport_peripheral_event transmitted[QUEUE_CAPACITY];

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
    assert(type == ESB_WIRE_EVENT_ZMK && lock_depth == 1U);
    assert(enqueue_calls < QUEUE_CAPACITY);
    transmitted[enqueue_calls++] = *event;
    return enqueue_result;
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
    transport_ready = 0;
    secure_state = ESB_V3_NO_SESSION;
    lock_depth = pressure_count = enqueue_calls = rekey_calls = 0U;
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
static void ready_queue_errors_keep_snapshot_recovery(void) {
    reset();
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    enqueue_result = -ENOSPC;
    assert(report(key(5U, true)) == -ENOSPC && held(5U));
    assert(key_state_work.scheduled == 1U && key_state_work.delay == K_NO_WAIT);
    assert(enqueue_calls == 1U && presession_events.used == 0U);
    assert(handshake_work.scheduled == 0U && flush_presession_work.scheduled == 0U);
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
    ready_queue_errors_keep_snapshot_recovery();
    ready_rekey_preserves_triggering_input();
    puts("8 peripheral startup input cases passed");
    return 0;
}
