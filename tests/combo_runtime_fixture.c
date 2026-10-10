/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model the ARM target's 32-bit long even on a 64-bit Linux host. */
#undef LONG_MAX
#define LONG_MAX INT32_MAX
#define MAX_COMBO_KEYS 2
#define BYTES_FOR_COMBOS_MASK 1
#define ZMK_KEYMAP_LEN 12
#define CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS 4
#define CONFIG_ZMK_SPLIT 1
#define IS_ENABLED(x) (x)
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL 255
#define ZMK_VIRTUAL_KEY_POSITION_COMBO(x) (100 + (x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BIT(x) (1u << (x))
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_CAPTURED 1
#define ZMK_EV_EVENT_HANDLED 2
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define __ASSERT(x, ...) CHECK(x)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define K_NO_WAIT 0
/* The pinned Zephyr Z_TIMEOUT_MS clamps negative milliseconds to zero. */
#define K_MSEC(x) ((x) < 0 ? 0 : (x))

typedef uintptr_t mem_addr_t;
typedef int zmk_event_t;
struct zmk_listener { int unused; };
struct zmk_position_state_changed { uint8_t source; uint32_t position; bool state; int64_t timestamp; };
struct zmk_position_state_changed_event { struct zmk_position_state_changed data; };
struct zmk_behavior_binding { int id; };
struct zmk_behavior_binding_event { uint32_t position; int64_t timestamp; uint8_t source; };
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; void (*handler)(struct k_work *); bool queued; int64_t delay; };

static int64_t now;
static unsigned normal_count, behavior_count, hook_calls, schedules;
static struct zmk_position_state_changed normal_events[32];
static struct { bool pressed; int id; int64_t timestamp; } behavior_events[16];
struct rx_item { int64_t timestamp; bool event; uint32_t position; bool pressed; };
static struct rx_item rx[32];
static unsigned rx_head, rx_tail;
static int64_t k_uptime_get(void) { return now; }
static int k_work_schedule(struct k_work_delayable *work, int64_t delay) {
    if (work->queued) return 0;
    CHECK(delay >= 0);
    work->queued = true;
    work->delay = delay;
    schedules++;
    return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) { work->queued = false; return 0; }
static void k_work_init_delayable(struct k_work_delayable *work, void (*handler)(struct k_work *)) {
    *work = (struct k_work_delayable){.handler = handler};
}
static bool totem_esb_rx_pending_before(int64_t deadline) {
    hook_calls++;
    return rx_head < rx_tail && rx[rx_head].timestamp <= deadline;
}
static uint8_t zmk_keymap_highest_layer_active(void) { return 0; }
static bool sys_bitfield_test_bit(mem_addr_t address, unsigned bit) {
    return (((uint32_t *)address)[bit / 32] & BIT(bit % 32)) != 0;
}
static void sys_bitfield_set_bit(mem_addr_t address, unsigned bit) { ((uint32_t *)address)[bit / 32] |= BIT(bit % 32); }
static void sys_bitfield_clear_bit(mem_addr_t address, unsigned bit) { ((uint32_t *)address)[bit / 32] &= ~BIT(bit % 32); }
static struct zmk_position_state_changed_event copy_raised_zmk_position_state_changed(const struct zmk_position_state_changed *event) {
    return (struct zmk_position_state_changed_event){.data = *event};
}
static void normal_event(struct zmk_position_state_changed event) {
    CHECK(normal_count < ARRAY_SIZE(normal_events));
    normal_events[normal_count++] = event;
}
static void route_event(struct zmk_position_state_changed event);
#define ZMK_EVENT_RELEASE(event) normal_event((event).data)
#define ZMK_EVENT_RAISE(event) route_event((event).data)
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event, bool pressed) {
    CHECK(behavior_count < ARRAY_SIZE(behavior_events));
    behavior_events[behavior_count].pressed = pressed;
    behavior_events[behavior_count].id = binding->id;
    behavior_events[behavior_count++].timestamp = event.timestamp;
    return 0;
}

/* ACTUAL_STRUCTURES */
static const struct combo_cfg combos[] = {
    {.key_positions = {2, 3}, .key_position_len = 2, .timeout_ms = 50, .behavior = {.id = 1}},
    {.key_positions = {4, 5}, .key_position_len = 2, .timeout_ms = 80, .behavior = {.id = 2}, .slow_release = true},
};
/* ACTUAL_COMBO_BODY */

static void route_event(struct zmk_position_state_changed event) {
    int result = event.state ? position_state_down(NULL, &event) : position_state_up(NULL, &event);
    CHECK(result >= 0);
    if (result == ZMK_EV_EVENT_BUBBLE) normal_event(event);
}
static void key(uint32_t position, bool pressed, int64_t timestamp) {
    route_event((struct zmk_position_state_changed){.source = 0, .position = position, .state = pressed, .timestamp = timestamp});
}
static void add_rx(int64_t timestamp, bool event, uint32_t position, bool pressed) {
    CHECK(rx_tail < ARRAY_SIZE(rx));
    rx[rx_tail++] = (struct rx_item){timestamp, event, position, pressed};
}
static void drain_rx_batch(void) {
    for (unsigned count = 0; count < 8 && rx_head < rx_tail; count++) {
        struct rx_item item = rx[rx_head++];
        if (item.event) key(item.position, item.pressed, item.timestamp);
    }
}
static void fire_timer(void) {
    CHECK(timeout_task.queued);
    timeout_task.queued = false; /* running work may enqueue itself behind RX */
    timeout_task.handler(&timeout_task.work);
}
static void reset(int64_t timestamp) {
    now = timestamp;
    normal_count = behavior_count = hook_calls = schedules = 0;
    rx_head = rx_tail = 0;
    pressed_keys_count = active_combo_count = 0;
    fully_pressed_combo = INT16_MAX;
    timeout_task_timeout_at = 0;
    last_tapped_timestamp = last_combo_timestamp = INT32_MIN;
    memset(pressed_keys, 0, sizeof(pressed_keys));
    memset(candidates, 0, sizeof(candidates));
    memset(combo_lookup, 0, sizeof(combo_lookup));
    memset(active_combos, 0, sizeof(active_combos));
    CHECK(combo_init() == 0);
}

int main(void) {
    unsigned scenarios = 0;
    /* Partner predates 50ms, but is behind two bounded eight-record RX batches. */
    reset(1250);
    key(2, true, 1000);
    CHECK(timeout_task_timeout_at == 1050 && timeout_task.delay == 0);
    for (unsigned i = 0; i < 16; i++) add_rx(1010 + i, false, 0, false);
    add_rx(1030, true, 3, true);
    for (unsigned batch = 0; batch < 3; batch++) {
        fire_timer();
        CHECK(normal_count == 0 && behavior_count == 0 && timeout_task.queued);
        CHECK(timeout_task_timeout_at == 1050 && timeout_task.delay == 0);
        drain_rx_batch();
    }
    CHECK(behavior_count == 1 && behavior_events[0].pressed && behavior_events[0].timestamp == 1000);
    CHECK(normal_count == 0 && !timeout_task.queued && hook_calls == 3);
    key(2, false, 1260); key(3, false, 1270);
    CHECK(behavior_count == 2 && !behavior_events[1].pressed && active_combo_count == 0);
    CHECK(normal_count == 0);
    scenarios++;

    /* Later incoming traffic cannot prolong the original deadline. */
    reset(1250);
    key(2, true, 1000);
    add_rx(1051, true, 3, true);
    fire_timer();
    CHECK(normal_count == 1 && normal_events[0].position == 2 && !timeout_task.queued);
    drain_rx_batch(); fire_timer();
    CHECK(normal_count == 2 && normal_events[1].position == 3 && behavior_count == 0);
    CHECK(timeout_task_timeout_at == 0 && !timeout_task.queued);
    scenarios++;

    /* Exact-boundary timestamps are still expired by the original strict > rule. */
    reset(1250);
    key(2, true, 1000); add_rx(1050, true, 3, true);
    fire_timer(); CHECK(normal_count == 0);
    drain_rx_batch();
    CHECK(normal_count == 1 && behavior_count == 0);
    fire_timer();
    CHECK(normal_count == 2 && behavior_count == 0 && !timeout_task.queued);
    scenarios++;

    /* A queued short release is forwarded once, preserving its actual timestamp. */
    reset(1250);
    key(2, true, 1000); add_rx(1030, true, 2, false);
    fire_timer(); CHECK(normal_count == 0);
    drain_rx_batch();
    CHECK(normal_count == 2 && normal_events[0].state && !normal_events[1].state);
    CHECK(normal_events[0].timestamp == 1000 && normal_events[1].timestamp == 1030);
    CHECK(!timeout_task.queued && behavior_count == 0);
    scenarios++;

    /* Ordinary empty/no-candidate paths cancel, including beyond signed-32 uptime. */
    reset((int64_t)INT32_MAX + 10000);
    CHECK(first_candidate_timeout() == LLONG_MAX);
    timeout_task_timeout_at = 123;
    CHECK(k_work_schedule(&timeout_task, 0) >= 0);
    update_timeout_task();
    CHECK(timeout_task_timeout_at == 0 && !timeout_task.queued);
    pressed_keys_count = 1;
    pressed_keys[0].data.timestamp = now;
    CHECK(first_candidate_timeout() == LLONG_MAX);
    update_timeout_task();
    CHECK(timeout_task_timeout_at == 0 && !timeout_task.queued);
    scenarios++;

    reset((int64_t)INT32_MAX + 10000);
    key(2, true, now);
    CHECK(timeout_task.delay == 50);
    now += 50; fire_timer();
    CHECK(normal_count == 1 && normal_events[0].timestamp == now - 50);
    CHECK(timeout_task_timeout_at == 0 && !timeout_task.queued);
    scenarios++;

    /* Filtered-out unrelated keys do not leave a stale timer or lose a press. */
    reset(1000);
    key(2, true, 1000); now = 1040; key(7, true, 1040);
    CHECK(normal_count == 2 && normal_events[0].position == 2 && normal_events[1].position == 7);
    CHECK(timeout_task_timeout_at == 0 && !timeout_task.queued && behavior_count == 0);
    scenarios++;

    /* The normal 50ms window and normal fast/slow combo release remain intact. */
    reset(1000);
    key(2, true, 1000); CHECK(timeout_task.delay == 50 && normal_count == 0);
    now = 1030; key(3, true, 1030);
    CHECK(behavior_count == 1 && behavior_events[0].id == 1);
    key(2, false, 1040); CHECK(behavior_count == 2 && !behavior_events[1].pressed);
    key(3, false, 1050); CHECK(behavior_count == 2 && active_combo_count == 0);
    scenarios++;

    reset(1000);
    key(4, true, 1000); CHECK(timeout_task.delay == 80);
    key(5, true, 1070); CHECK(behavior_count == 1 && behavior_events[0].id == 2);
    key(4, false, 1080); CHECK(behavior_count == 1);
    key(5, false, 1090); CHECK(behavior_count == 2 && !behavior_events[1].pressed);
    CHECK(normal_count == 0 && active_combo_count == 0);
    scenarios++;

    /* Combo output and prior-idle bookkeeping retain the complete 64-bit uptime. */
    reset((int64_t)INT32_MAX + 10000);
    key(2, true, now); key(3, true, now + 30);
    CHECK(behavior_count == 1 && behavior_events[0].timestamp == now);
    CHECK(last_combo_timestamp == now);
    store_last_tapped(now - 1);
    CHECK(last_tapped_timestamp == INT32_MIN);
    key(2, false, now + 40); key(3, false, now + 50);
    CHECK(behavior_count == 2 && behavior_events[1].timestamp == now + 40);
    CHECK(normal_count == 0 && active_combo_count == 0);
    scenarios++;
    printf("%u actual combo scenarios passed\n", scenarios);
    return 0;
}
