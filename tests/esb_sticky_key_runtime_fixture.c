/* SPDX-License-Identifier: MIT
 * Actual sticky-key state transitions with modeled workqueue/event boundaries.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "sticky assertion failed: %s:%d (%s): %s\n", \
                __FILE__, __LINE__, __func__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define IS_ENABLED(option) (option)
#define CONFIG_ZMK_SPLIT 1
#define ZMK_BHV_STICKY_KEY_MAX_HELD 4
#define ZMK_BHV_STICKY_KEY_POSITION_FREE UINT32_MAX
#define KEY_PRESS "kp"
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_CAPTURED 1
#define K_NO_WAIT 0
#define K_MSEC(value) (value)
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define ZMK_HID_USAGE_ID(value) ((value) & 0xffffU)
#define ZMK_HID_USAGE_PAGE(value) (((value) >> 16) & 0xffU)
#define SELECT_MODS(value) (((value) >> 24) & 0xffU)
#define ZMK_EVENT_RAISE_AFTER(event, listener) reraise(&(event))

struct k_work { bool running; };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };
struct device { const void *config; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { uint32_t position; int64_t timestamp; uint8_t source; };
struct zmk_keycode_state_changed {
    uint8_t usage_page, implicit_modifiers;
    uint32_t keycode;
    bool state;
    int64_t timestamp;
};
typedef struct zmk_keycode_state_changed zmk_event_t;
struct zmk_keycode_state_changed_event { struct zmk_keycode_state_changed data; };

/* ACTUAL_STRUCTURES */

static struct active_sticky_key active_sticky_keys[ZMK_BHV_STICKY_KEY_MAX_HELD];
static struct behavior_sticky_key_config test_config;
static struct device test_device = { .config = &test_config };
static const char binding_name[] = "sticky";
static const char child_name[] = "mo";
static struct zmk_behavior_binding test_binding = { .behavior_dev = binding_name, .param1 = 6 };
static int64_t now, pending_rx_at, last_release_at;
static bool rx_pending, held, cancel_in_progress;
static unsigned int press_count, release_count, delivered_events, pending_queries;

static int64_t k_uptime_get(void) { return now; }
static int k_work_schedule(struct k_work_delayable *work, int64_t delay) {
    if (work->pending) { return 0; }
    work->pending = true;
    work->due = now + delay;
    return work->work.running && delay == K_NO_WAIT ? 2 : 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    if (cancel_in_progress) {
        cancel_in_progress = false;
        return -EINPROGRESS;
    }
    work->pending = false;
    return 0;
}
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static bool totem_esb_rx_pending_before(int64_t deadline) {
    pending_queries++;
    return rx_pending && pending_rx_at <= deadline;
}
static const struct device *zmk_behavior_get_binding(const char *name) {
    assert(name == binding_name);
    return &test_device;
}
static int zmk_behavior_invoke_binding(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event, bool pressed) {
    assert(binding->behavior_dev == child_name && binding->param1 == 6);
    assert(event.position == 3 && event.source == 1);
    assert(held != pressed);
    held = pressed;
    if (pressed) { press_count++; }
    else { release_count++; last_release_at = event.timestamp; }
    return 0;
}
static struct zmk_keycode_state_changed *as_zmk_keycode_state_changed(const zmk_event_t *event) {
    return (struct zmk_keycode_state_changed *)event;
}
static struct zmk_keycode_state_changed_event copy_raised_zmk_keycode_state_changed(
    const struct zmk_keycode_state_changed *event) {
    return (struct zmk_keycode_state_changed_event){ .data = *event };
}
static bool is_mod(uint8_t page, uint32_t key) { return page == 7 && key >= 224 && key <= 231; }
static void reraise(const struct zmk_keycode_state_changed_event *event) {
    assert(held && event->data.usage_page == 7 && event->data.keycode == 4);
    delivered_events++;
}

/* ACTUAL_CALLBACKS */

static struct zmk_behavior_binding_event binding_event(int64_t timestamp) {
    return (struct zmk_behavior_binding_event){ .position = 3, .source = 1, .timestamp = timestamp };
}
static void reset_fixture(bool lazy, bool quick) {
    memset(active_sticky_keys, 0, sizeof(active_sticky_keys));
    for (int i = 0; i < ZMK_BHV_STICKY_KEY_MAX_HELD; i++) {
        active_sticky_keys[i].position = ZMK_BHV_STICKY_KEY_POSITION_FREE;
    }
    test_config = (struct behavior_sticky_key_config){
        .release_after_ms = 100, .lazy = lazy, .quick_release = quick,
        .behavior.behavior_dev = child_name,
    };
    now = 1000;
    pending_rx_at = last_release_at = 0;
    rx_pending = held = cancel_in_progress = false;
    press_count = release_count = delivered_events = pending_queries = 0;
}
static void tap_sticky(int64_t press_at, int64_t release_at) {
    assert(on_sticky_key_binding_pressed(&test_binding, binding_event(press_at)) == 0);
    assert(on_sticky_key_binding_released(&test_binding, binding_event(release_at)) == 0);
}
static void fire_timer(void) {
    struct k_work_delayable *work = &active_sticky_keys[0].release_timer;
    assert(work->pending && work->due <= now);
    work->pending = false;
    work->work.running = true;
    behavior_sticky_key_timer_handler(&work->work);
    work->work.running = false;
}
static int key_event(bool pressed, int64_t timestamp) {
    const zmk_event_t event = { .usage_page = 7, .keycode = 4,
                              .state = pressed, .timestamp = timestamp };
    return sticky_key_keycode_state_changed_listener(&event);
}

int main(void) {
    /* Normal release: the configured deadline is unchanged. */
    reset_fixture(false, false);
    tap_sticky(980, 1000);
    assert(held && active_sticky_keys[0].release_timer.due == 1100);
    now = 1100;
    fire_timer();
    assert(!held && release_count == 1 && last_release_at == 1100);

    /* Ingress timestamp already expired by dispatch: expiry must still run. */
    reset_fixture(false, false);
    tap_sticky(700, 750);
    assert(active_sticky_keys[0].release_timer.pending);
    assert(active_sticky_keys[0].release_timer.due == now);
    fire_timer();
    assert(!held && last_release_at == 850);

    /* Exactly due is also an immediate timer, not an unarmed modifier. */
    reset_fixture(false, false);
    tap_sticky(850, 900);
    fire_timer();
    assert(!held && last_release_at == 1000);

    /* Earlier RX waits behind timer: yield, let input consume sticky layer. */
    reset_fixture(false, false);
    tap_sticky(700, 750);
    rx_pending = true;
    pending_rx_at = 840;
    fire_timer();
    assert(held && release_count == 0 && active_sticky_keys[0].release_timer.pending);
    rx_pending = false;
    assert(key_event(true, 840) == ZMK_EV_EVENT_BUBBLE);
    assert(held && !active_sticky_keys[0].release_timer.pending);
    assert(key_event(false, 1100) == ZMK_EV_EVENT_CAPTURED);
    assert(!held && delivered_events == 1 && last_release_at == 1100);

    /* Existing inclusive expiry semantics: a key at deadline still uses it. */
    reset_fixture(false, true);
    tap_sticky(700, 750);
    rx_pending = true;
    pending_rx_at = 850;
    fire_timer();
    assert(held && release_count == 0);
    rx_pending = false;
    assert(key_event(true, 850) == ZMK_EV_EVENT_CAPTURED);
    assert(!held && delivered_events == 1 && last_release_at == 850);

    /* Later arrivals cannot keep a real timeout alive indefinitely. */
    reset_fixture(false, false);
    tap_sticky(700, 750);
    rx_pending = true;
    pending_rx_at = 851;
    fire_timer();
    assert(!held && !active_sticky_keys[0].release_timer.pending);
    assert(key_event(true, 851) == ZMK_EV_EVENT_BUBBLE);
    assert(release_count == 1 && last_release_at == 850);

    /* Lazy sticky modifier/layer expires without sending an unmatched release. */
    reset_fixture(true, true);
    tap_sticky(700, 750);
    fire_timer();
    assert(!held && press_count == 0 && release_count == 0);
    assert(active_sticky_keys[0].position == ZMK_BHV_STICKY_KEY_POSITION_FREE);

    /* Lazy quick-release wraps an earlier queued key once and in order. */
    reset_fixture(true, true);
    tap_sticky(700, 750);
    rx_pending = true;
    pending_rx_at = 840;
    fire_timer();
    assert(!held && active_sticky_keys[0].release_timer.pending);
    rx_pending = false;
    assert(key_event(true, 840) == ZMK_EV_EVENT_CAPTURED);
    assert(!held && press_count == 1 && release_count == 1 && delivered_events == 1);

    /* Preserve the pinned handler's defensive cancellation state. The pinned
     * Zephyr cancel API returns nonnegative busy flags, so the injected legacy
     * -EINPROGRESS path is not claimed to be a real same-workqueue outcome. */
    reset_fixture(false, false);
    tap_sticky(700, 750);
    cancel_in_progress = true;
    assert(key_event(true, 840) == ZMK_EV_EVENT_BUBBLE);
    assert(active_sticky_keys[0].timer_cancelled);
    rx_pending = true;
    pending_rx_at = 840;
    fire_timer();
    assert(held && !active_sticky_keys[0].timer_cancelled && pending_queries == 0);
    assert(!active_sticky_keys[0].release_timer.pending);
    rx_pending = false;
    assert(key_event(false, 1050) == ZMK_EV_EVENT_CAPTURED);
    assert(!held);

    /* Already cleared slot: a stale callback must not revive it for RX. */
    reset_fixture(false, false);
    rx_pending = true;
    pending_rx_at = 10;
    behavior_sticky_key_timer_handler(&active_sticky_keys[0].release_timer.work);
    assert(pending_queries == 0 && !active_sticky_keys[0].release_timer.pending);

    /* Bounded RX may need several workqueue batches. Yield until the old
     * record drains; then expire even if that record did not produce a key. */
    reset_fixture(false, false);
    tap_sticky(700, 750);
    rx_pending = true;
    pending_rx_at = 800;
    for (unsigned int batch = 0; batch < 3; batch++) {
        fire_timer();
        assert(held && active_sticky_keys[0].release_timer.pending);
    }
    rx_pending = false;
    fire_timer();
    assert(!held && release_count == 1 && last_release_at == 850);

    puts("11 sticky ingress/expiry cases passed");
    return 0;
}
