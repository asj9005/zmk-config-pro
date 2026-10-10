/* SPDX-License-Identifier: MIT
 * Actual pinned tap-dance C with deterministic behavior, clock and work fakes.
 * The RX hook models the oldest ingress deadline; its actual FIFO implementation
 * is covered by test_esb_rx_runtime and test_hold_tap_runtime.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define CONFIG_ZMK_SPLIT 1
#define CONFIG_ZMK_BEHAVIOR_METADATA 0
#define CONFIG_ZMK_BEHAVIOR_TAP_DANCE_MAX_HELD 4
#define IS_ENABLED(x) (x)
#define DT_HAS_COMPAT_STATUS_OKAY(x) 1
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define ZMK_BEHAVIOR_OPAQUE 1
#define ZMK_EV_EVENT_BUBBLE 0
#define K_MSEC(ms) ((ms) > 0 ? (ms) : 0)
#define K_NO_WAIT 0
#define CONTAINER_OF(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))
static const char *scenario;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CHECK failed %s:%d: %s\n", scenario, __LINE__, #expr); exit(87); } } while (0)
static int64_t now, oldest_rx;
static bool rx_pending;
static unsigned int scheduled, requeued, scenarios;
static int64_t k_uptime_get(void) { return now; }
static bool totem_esb_rx_pending_before(int64_t deadline) { return rx_pending && oldest_rx <= deadline; }
struct k_work { void (*handler)(struct k_work *); bool running; };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static void k_work_init_delayable(struct k_work_delayable *work, void (*handler)(struct k_work *)) {
    *work = (struct k_work_delayable){.work = {.handler = handler}};
}
static int k_work_schedule(struct k_work_delayable *work, int64_t delay) {
    CHECK(delay >= 0);
    if (work->pending) return 0;
    work->pending = true; work->due = now + delay; scheduled++;
    if (delay == K_NO_WAIT && work->work.running) { requeued++; return 2; }
    return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) { work->pending = false; return 0; }
struct device { const void *config; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { uint32_t position; int64_t timestamp; uint8_t source; };
struct zmk_position_state_changed { uint32_t position; int64_t timestamp; bool state; };
typedef struct zmk_position_state_changed zmk_event_t;
struct zmk_listener { int (*callback)(const zmk_event_t *); };
#define ZMK_LISTENER(name, fn) const struct zmk_listener zmk_listener_##name = {.callback = fn}
#define ZMK_SUBSCRIPTION(...)
static struct zmk_position_state_changed *as_zmk_position_state_changed(const zmk_event_t *ev) {
    return (struct zmk_position_state_changed *)ev;
}
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};
static const struct device *zmk_behavior_get_binding(const char *name);
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *, struct zmk_behavior_binding_event, bool);

/* ACTUAL_TAP_DANCE_SOURCE */

static struct zmk_behavior_binding choices[] = {{"key", 1, 0}, {"key", 2, 0}};
static struct behavior_tap_dance_config config = {.tapping_term_ms = 200, .behavior_count = 2, .behaviors = choices};
static struct device device = {.config = &config};
static struct zmk_behavior_binding binding = {.behavior_dev = "dance"};
struct delivery { uint32_t code; bool pressed; int64_t timestamp; uint8_t source; };
static struct delivery deliveries[32];
static unsigned int delivery_count;
static int held[3];
static const struct device *zmk_behavior_get_binding(const char *name) { CHECK(strcmp(name, "dance") == 0); return &device; }
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *child,
                                      struct zmk_behavior_binding_event event, bool pressed) {
    CHECK(child->param1 > 0 && child->param1 < 3 && delivery_count < 32);
    held[child->param1] += pressed ? 1 : -1;
    CHECK(held[child->param1] >= 0 && held[child->param1] <= 1);
    deliveries[delivery_count++] = (struct delivery){child->param1, pressed, event.timestamp, event.source};
    return 0;
}
static void reset(const char *name) {
    CHECK(held[1] == 0 && held[2] == 0);
    scenario = name; scenarios++; now = 1000; oldest_rx = 0; rx_pending = false;
    scheduled = requeued = delivery_count = 0;
    memset(deliveries, 0, sizeof(deliveries));
    for (unsigned int i = 0; i < CONFIG_ZMK_BEHAVIOR_TAP_DANCE_MAX_HELD; i++) {
        memset(&active_tap_dances[i], 0, sizeof(active_tap_dances[i]));
        k_work_init_delayable(&active_tap_dances[i].release_timer, behavior_tap_dance_timer_handler);
        clear_tap_dance(&active_tap_dances[i]);
    }
}
static void edge(bool pressed, int64_t timestamp, int64_t processing_time) {
    now = processing_time;
    struct zmk_behavior_binding_event event = {.position = 37, .source = 1, .timestamp = timestamp};
    if (pressed) on_tap_dance_binding_pressed(&binding, event);
    else on_tap_dance_binding_released(&binding, event);
}
static void fire(struct active_tap_dance *dance, int64_t time) {
    CHECK(dance != NULL && dance->release_timer.pending);
    now = time; dance->release_timer.pending = false; dance->release_timer.work.running = true;
    behavior_tap_dance_timer_handler(&dance->release_timer.work);
    dance->release_timer.work.running = false;
}
static void expect_single(unsigned int offset, int64_t timestamp) {
    CHECK(deliveries[offset].code == 1 && deliveries[offset].pressed);
    CHECK(deliveries[offset + 1].code == 1 && !deliveries[offset + 1].pressed);
    CHECK(deliveries[offset].timestamp == timestamp && deliveries[offset + 1].timestamp == timestamp);
    CHECK(deliveries[offset].source == 1 && deliveries[offset + 1].source == 1);
}
int main(void) {
    reset("normal_single_tap");
    edge(true, 1000, 1000); edge(false, 1050, 1050);
    CHECK(delivery_count == 0); fire(find_tap_dance(37), 1200);
    CHECK(delivery_count == 2 && find_tap_dance(37) == NULL); expect_single(0, 1200);

    reset("normal_double_tap");
    edge(true, 1000, 1000); edge(false, 1050, 1050);
    edge(true, 1100, 1100); edge(false, 1150, 1150);
    CHECK(delivery_count == 2 && deliveries[0].code == 2 && held[2] == 0);
    CHECK(find_tap_dance(37) == NULL);

    reset("overdue_press_still_schedules_completion");
    edge(true, 1000, 1400); edge(false, 1050, 1400);
    struct active_tap_dance *dance = find_tap_dance(37);
    CHECK(dance != NULL && dance->release_timer.pending && dance->release_timer.due == 1400);
    fire(dance, 1400); CHECK(delivery_count == 2); expect_single(0, 1200);

    reset("timer_first_preserves_already_arrived_second_tap");
    edge(true, 1000, 1000); edge(false, 1050, 1050);
    oldest_rx = 1100; rx_pending = true; fire(find_tap_dance(37), 1400);
    CHECK(delivery_count == 0 && requeued == 1);
    rx_pending = false; edge(true, 1100, 1400); edge(false, 1150, 1400);
    CHECK(delivery_count == 2 && deliveries[0].code == 2 && held[2] == 0);

    reset("newer_packets_do_not_extend_dance_deadline");
    edge(true, 1000, 1000); edge(false, 1050, 1050);
    oldest_rx = 1201; rx_pending = true; fire(find_tap_dance(37), 1400);
    CHECK(delivery_count == 2 && requeued == 0); expect_single(0, 1200);

    for (int64_t gap = 200; gap <= 201; gap++) {
        reset("late_same_position_is_two_singles_in_one_rx_batch");
        edge(true, 1000, 1600); edge(false, 1050, 1600);
        edge(true, 1000 + gap, 1600);
        CHECK(delivery_count == 2 && find_tap_dance(37)->counter == 1);
        expect_single(0, 1200); edge(false, 1050 + gap, 1600);
        fire(find_tap_dance(37), 1600); CHECK(delivery_count == 4); expect_single(2, 1200 + gap);
    }

    reset("held_dance_releases_on_original_edge");
    edge(true, 1000, 1000); fire(find_tap_dance(37), 1200);
    CHECK(delivery_count == 1 && held[1] == 1);
    edge(false, 1300, 1300); CHECK(delivery_count == 2 && held[1] == 0);

    reset("cancelled_timer_is_not_requeued");
    edge(true, 1000, 1000); edge(false, 1050, 1050);
    dance = find_tap_dance(37); dance->timer_cancelled = true;
    oldest_rx = 1100; rx_pending = true; fire(dance, 1400);
    CHECK(delivery_count == 0 && !dance->release_timer.pending && requeued == 0);
    dance->timer_cancelled = false; rx_pending = false;
    k_work_schedule(&dance->release_timer, K_NO_WAIT); fire(dance, 1400); expect_single(0, 1200);

    reset("free_timer_is_not_requeued");
    dance = &active_tap_dances[0]; k_work_schedule(&dance->release_timer, K_NO_WAIT);
    oldest_rx = 1100; rx_pending = true; fire(dance, 1400);
    CHECK(delivery_count == 0 && !dance->release_timer.pending && requeued == 0);

    printf("%u actual tap-dance timing scenarios passed\n", scenarios);
    return 0;
}
