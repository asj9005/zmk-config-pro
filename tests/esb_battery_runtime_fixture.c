/* SPDX-License-Identifier: MIT
 * Insert production callbacks unchanged; model only their external boundaries.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "battery assertion failed: %s:%d (%s): %s\n", \
                __FILE__, __LINE__, __func__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define IS_ENABLED(option) (option)
#define CONFIG_TOTEM_ESB_V3 1
#define CONFIG_ZMK_BATTERY_REPORTING 1
#define CONFIG_ZMK_BATTERY_REPORT_INTERVAL 60
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 64
#define CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS 2
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT 1
#define ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT 2
#define ESB_WIRE_EVENT_ZMK 0
#define ESB_WIRE_COMMAND_V3_SESSION_OK 4
#define BIT(bit) (1U << (bit))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(ms) (ms)
#define K_SECONDS(s) ((s) * 1000)
#define ARG_UNUSED(value) ((void)(value))
enum esb_v3_peripheral_state {
    ESB_V3_NO_SESSION, ESB_V3_WAIT_CHALLENGE,
    ESB_V3_WAIT_SESSION_OK, ESB_V3_ESTABLISHED,
};
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
struct fake_envelope {
    struct { int wire_type; uint32_t sequence; uint64_t session_id; } payload;
};
static struct k_mutex event_mutex;
static struct k_work_delayable battery_refresh_work, handshake_work;
static struct k_work_delayable flush_presession_work, key_state_work;
static struct { unsigned int used; } presession_events;
static struct { uint8_t keys[8]; } local_key_state;
static int handshake_backoff, transport_ready;
static enum esb_v3_peripheral_state secure_state;
static uint8_t cached_battery_level, sent_level;
static bool cached_battery_valid;
static uint32_t wire_sequence, downlink_sequence;
static int64_t wait_session_ok_started_at;
static uint64_t wire_session_id = 42, active_session;
static unsigned int lock_depth, enqueue_calls, admitted_calls, key_calls;
static int enqueue_result, activate_result;

#define atomic_get(value) (*(value))
static enum esb_v3_peripheral_state get_secure_state(void) { return secure_state; }
static void set_secure_state(enum esb_v3_peripheral_state state) { secure_state = state; }
static int k_mutex_lock(struct k_mutex *mutex, int timeout) {
    assert(mutex == &event_mutex && timeout == K_FOREVER && lock_depth++ == 0);
    return 0;
}
static int k_mutex_unlock(struct k_mutex *mutex) {
    assert(mutex == &event_mutex && lock_depth == 1);
    lock_depth--;
    return 0;
}
static unsigned int k_msgq_num_used_get(const void *queue) {
    assert(queue == &presession_events && lock_depth == 1);
    return presession_events.used;
}
static int k_msgq_put(void *queue, const void *event, int timeout) {
    assert(queue == &presession_events && event != NULL && timeout == K_NO_WAIT);
    assert(lock_depth == 1);
    presession_events.used++;
    return 0;
}
static int k_work_reschedule(struct k_work_delayable *work, int delay) {
    work->scheduled++;
    work->delay = delay;
    return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    assert(work == &handshake_work);
    return 0;
}
static void totem_esb_transport_queue_pressure(bool presession) {
    (void)presession;
    assert(false);
}
static void totem_esb_backoff_reset(void *backoff, unsigned int delay) {
    assert(backoff == &handshake_backoff);
    assert(delay == CONFIG_TOTEM_ESB_V3_HANDSHAKE_INTERVAL_MS);
}
static int report_event_locked(const struct zmk_split_transport_peripheral_event *event) {
    assert(event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT);
    assert(lock_depth == 1);
    key_calls++;
    return 0;
}
static int enqueue_wire_event(int type, const struct zmk_split_transport_peripheral_event *event) {
    assert(type == ESB_WIRE_EVENT_ZMK && lock_depth == 1);
    assert(event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT);
    enqueue_calls++;
    if (enqueue_result == 0) {
        sent_level = event->data.battery_event.level;
        admitted_calls++;
    }
    return enqueue_result;
}
static uint64_t totem_esb_v3_active_session(uint8_t source) {
    assert(source == 0);
    return active_session;
}
static int totem_esb_v3_activate_pending(uint8_t source) {
    assert(source == 0 && lock_depth == 1);
    if (activate_result == 0) { active_session = wire_session_id; }
    return activate_result;
}

/* ACTUAL_FIRMWARE_FUNCTIONS */

static int report_battery(uint8_t level) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
        .data.battery_event.level = level,
    };
    return split_peripheral_esb_report_event(&event);
}
static void refresh(void) {
    battery_refresh_work.scheduled = 0;
    battery_refresh_work_cb(NULL);
    assert(lock_depth == 0);
}
static int establish(void) {
    const struct fake_envelope env = {
        .payload = {.wire_type = ESB_WIRE_COMMAND_V3_SESSION_OK, .session_id = 42},
    };
    return receive_session_ok(&env);
}
int main(void) {
    /* No sample exists, even if secure transport is already established. */
    transport_ready = 1;
    secure_state = ESB_V3_ESTABLISHED;
    refresh();
    assert(enqueue_calls == 0 && battery_refresh_work.scheduled == 0);

    /* A real sample before radio startup is retained outside the key queue. */
    transport_ready = 0;
    secure_state = ESB_V3_WAIT_SESSION_OK;
    assert(report_battery(55) == 0 && cached_battery_valid && cached_battery_level == 55);
    assert(presession_events.used == 0 && battery_refresh_work.delay == 1000);
    refresh();
    assert(enqueue_calls == 0 && battery_refresh_work.scheduled == 0);

    /* Authentication failure cannot start a replay; successful SESSION_OK can. */
    transport_ready = 1;
    activate_result = -EACCES;
    assert(establish() == -EACCES && battery_refresh_work.scheduled == 0);
    activate_result = 0;
    assert(establish() == 0 && battery_refresh_work.scheduled == 1);
    assert(battery_refresh_work.delay == 1000);
    assert(establish() == 0 && battery_refresh_work.scheduled == 1); /* duplicate */

    /* Ordered key edges always take priority over battery metadata. */
    presession_events.used = 2;
    refresh();
    assert(enqueue_calls == 0 && presession_events.used == 2);
    assert(battery_refresh_work.scheduled == 1 && battery_refresh_work.delay == 1000);
    presession_events.used = 0;
    refresh();
    assert(admitted_calls == 1 && sent_level == 55);
    assert(battery_refresh_work.delay == 60000);

    /* ACKed/admitted-but-lost data converges on a later unchanged refresh. */
    refresh();
    assert(admitted_calls == 2 && sent_level == 55 && battery_refresh_work.delay == 60000);

    /* Dongle-only reboot: unchanged half sample replays in the new session. */
    secure_state = ESB_V3_WAIT_SESSION_OK;
    refresh();
    assert(battery_refresh_work.scheduled == 0);
    assert(establish() == 0 && battery_refresh_work.scheduled == 1);
    assert(battery_refresh_work.delay == 1000);
    refresh();
    assert(admitted_calls == 3 && sent_level == 55);

    /* Radio backlog retries slowly without consuming a key-queue entry. */
    enqueue_result = -ENOSPC;
    refresh();
    assert(admitted_calls == 3 && battery_refresh_work.delay == 1000);
    assert(presession_events.used == 0);
    enqueue_result = -EOVERFLOW;
    refresh();
    assert(admitted_calls == 3 && battery_refresh_work.delay == 1000);
    enqueue_result = 0;

    /* Multiple measurements coalesce; the latest valid value wins. */
    assert(report_battery(72) == 0 && report_battery(73) == 0);
    assert(report_battery(101) == -EINVAL && cached_battery_level == 73);
    refresh();
    assert(admitted_calls == 4 && sent_level == 73);

    /* A measured empty battery is distinct from no measurement. */
    assert(report_battery(0) == 0);
    refresh();
    assert(admitted_calls == 5 && sent_level == 0);

    /* Ordinary key admission and the held-key bitmap remain unchanged. */
    const struct zmk_split_transport_peripheral_event key = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = 3, .pressed = true},
    };
    assert(split_peripheral_esb_report_event(&key) == 0);
    assert(key_calls == 1 && local_key_state.keys[0] == BIT(3) && lock_depth == 0);
    puts("10 battery cache/reconnect cases passed");
    return 0;
}
