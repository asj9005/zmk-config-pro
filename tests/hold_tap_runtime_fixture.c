/* SPDX-License-Identifier: MIT
 * Actual hold-tap C (ZMK 904c9aec + Totem overlay), AutoBase and central helpers.
 * The event boundary preserves synchronous capture/replay and nested dispatch;
 * kernel work timing is deliberately fake and never runs a timer inside replay.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "esb_key_state.h"
#define CONFIG_TOTEM_ESB_DIAGNOSTICS 1
#include "esb_diagnostics.h"

#define CONFIG_ZMK_SPLIT 1
#define CONFIG_ZMK_BEHAVIOR_METADATA 0
#define CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_HELD 10
#define CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_CAPTURED_EVENTS 40
#define CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT 2
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 38
#define ZMK_KEYMAP_LEN 38
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL UINT8_MAX
#define IS_ENABLED(value) (value)
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_CAPTURED 2
#define BIT(bit) (1U << (bit))
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
/* Pinned Zephyr Z_TIMEOUT_MS clamps a negative relative delay to zero. */
#define K_MSEC(ms) ((ms) > 0 ? (ms) : 0)
#define K_NO_WAIT 0
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed %s:%d: %s\n", scenario, __LINE__, #condition); exit(87); \
} } while (0)

static const char *scenario = "initialization";
static int64_t now;
static unsigned sleep_calls, overflow_count, dispatch_depth, maximum_dispatch_depth;
static unsigned scenario_count;
static bool cancel_in_progress;
struct k_work { void (*handler)(struct k_work *); bool running; };
struct k_work_delayable { struct k_work work; bool pending; int64_t due; };
static int64_t k_uptime_get(void) { return now; }
static int k_msleep(int ms) { sleep_calls++; now += ms; return 0; }
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static void k_work_init_delayable(struct k_work_delayable *work, void (*handler)(struct k_work *)) {
    *work = (struct k_work_delayable){.work = {.handler = handler}};
}
static int k_work_schedule(struct k_work_delayable *work, int64_t delay) {
    if (work->pending) return 0;
    work->pending = true; work->due = now + delay;
    /* Pinned Zephyr work.c allows a RUNNING item to queue itself again on the
     * same workqueue; K_NO_WAIT returns2 in that case. It is not reentrant. */
    return delay == K_NO_WAIT && work->work.running ? 2 : 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    if (cancel_in_progress) { cancel_in_progress = false; return -EINPROGRESS; }
    work->pending = false; return 0;
}

/* Small local RX metadata FIFO. The production peek/validation/time-expansion
 * functions and central hook below are extracted unchanged. RF authentication
 * is represented by the payload marker; IRQ and work scheduling remain fake. */
#define CONFIG_ESB_PIPE_COUNT 3
#define CONFIG_ESB_MAX_PAYLOAD_LENGTH 64
struct k_spinlock { bool held; };
typedef unsigned int k_spinlock_key_t;
static unsigned int rx_locks, rx_corrupt_count;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    CHECK(!lock->held && rx_locks == 0); lock->held = true; rx_locks++; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    (void)key; CHECK(lock->held && rx_locks == 1); lock->held = false; rx_locks--;
}
struct ring_buf { uint8_t data[1024]; size_t size; };
static struct ring_buf timed_rx;
static size_t ring_buf_size_get(const struct ring_buf *ring) { return ring->size; }
static bool ring_buf_is_empty(const struct ring_buf *ring) { return ring->size == 0; }
static void ring_buf_reset(struct ring_buf *ring) { ring->size = 0; }
static size_t ring_buf_peek(const struct ring_buf *ring, uint8_t *out, size_t size) {
    if (size > ring->size) size = ring->size;
    memcpy(out, ring->data, size); return size;
}
#pragma pack(push, 1)
/* ACTUAL_RX_RECORD */
#pragma pack(pop)
struct zmk_split_esb_state {
    struct ring_buf *rx_buf;
    uint8_t rx_first_pipe, rx_last_pipe;
    uint32_t rx_pipe_bytes[CONFIG_ESB_PIPE_COUNT];
    struct k_spinlock rx_lock;
};
static struct zmk_split_esb_state state = {.rx_buf = &timed_rx, .rx_first_pipe = 1, .rx_last_pipe = 2};
static int diag_frame_result(int error) { CHECK(rx_locks == 0); rx_corrupt_count++; return error; }
/* ACTUAL_RX_TIMING_HELPERS */
static bool totem_esb_rx_pending_before(int64_t deadline);
void totem_esb_diag_event(enum totem_esb_diag_event event, int value) {
    (void)value;
    if (event == TOTEM_DIAG_HOLD_TAP_OVERFLOW) overflow_count++;
}

struct device { const void *config; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { int layer; uint32_t position; int64_t timestamp; uint8_t source; };
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};
struct zmk_event_type { int id; };
typedef struct { const struct zmk_event_type *event; uint8_t last_listener_index; } zmk_event_t;
struct zmk_listener { int (*callback)(const zmk_event_t *); };
struct zmk_position_state_changed { uint8_t source; uint32_t position; bool state; int64_t timestamp; };
struct zmk_keycode_state_changed {
    uint16_t usage_page; uint32_t keycode; uint8_t implicit_modifiers, explicit_modifiers;
    bool state; int64_t timestamp;
};
#define EVENT_TYPE(type, id_value) \
    static const struct zmk_event_type zmk_event_##type = {.id = id_value}; \
    struct type##_event { zmk_event_t header; struct type data; }; \
    static struct type *as_##type(const zmk_event_t *event) { \
        return event->event == &zmk_event_##type ? &((struct type##_event *)event)->data : NULL; \
    } \
    static struct type##_event copy_raised_##type(const struct type *data) { \
        return *CONTAINER_OF(data, struct type##_event, data); \
    }
EVENT_TYPE(zmk_position_state_changed, 1)
EVENT_TYPE(zmk_keycode_state_changed, 2)
#define ZMK_LISTENER(name, callback_fn) \
    const struct zmk_listener zmk_listener_##name = {.callback = callback_fn};
#define ZMK_SUBSCRIPTION(...)
#define ZMK_EVENT_RAISE_AT(ev, listener) \
    zmk_event_manager_raise_at(&(ev).header, &zmk_listener_##listener)
static bool is_mod(uint16_t page, uint32_t code) { return page == 7 && code >= 224 && code <= 231; }
static int zmk_event_manager_raise_at(zmk_event_t *, const struct zmk_listener *);
static const struct device *zmk_behavior_get_binding(const char *);
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *, struct zmk_behavior_binding_event, bool);
static int raise_zmk_position_state_changed(struct zmk_position_state_changed);
static int raise_zmk_keycode_state_changed_from_encoded(uint32_t, bool, int64_t);
static int zmk_keymap_layer_to(int, bool);

/* ACTUAL_HOLD_TAP_SOURCE */

/* ACTUAL_AUTO_BASE_HELPERS */

static struct behavior_hold_tap_config configurations[16];
static struct device devices[16];
static char device_names[16][12];
static struct zmk_behavior_binding mapping[38], pressed_mapping[38];
static bool physical_bindings[38];
static bool mouse_layer;
static int layer_refs[16], code_refs[256], mouse_refs;
static unsigned hold_press_count, hold_release_count, tap_press_count, tap_release_count;
struct delivery { uint32_t position; uint8_t source; bool state; int64_t timestamp; };
static struct delivery deliveries[4096];
static size_t delivery_count;
struct code_delivery { uint32_t keycode; bool state; };
static struct code_delivery codes[4096];
static size_t code_count;
static bool overwrite_replay_slot, fail_tap_press;
static uint8_t key_pos_states[2][5];
static int esb_central;
static const int *active_transport = &esb_central;
enum { ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT = 1 };
struct zmk_split_transport_peripheral_event {
    uint8_t type;
    union { struct { uint32_t position; bool pressed; } key_position_event; } data;
};
static int zmk_split_transport_central_peripheral_event_handler(
    const int *transport, uint8_t source, struct zmk_split_transport_peripheral_event event) {
    CHECK(transport == &esb_central);
    return raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = source, .position = event.data.key_position_event.position,
        .state = event.data.key_position_event.pressed, .timestamp = now});
}
static void totem_owned_swapper_source_reset(uint8_t source) { (void)source; }

/* ACTUAL_CENTRAL_HELPERS */
/* ACTUAL_CENTRAL_TIMING_HOOK */

static const struct device *zmk_behavior_get_binding(const char *name) {
    for (size_t i = 0; i < 16; i++) if (strcmp(name, device_names[i]) == 0) return &devices[i];
    CHECK(false); return NULL;
}
static int zmk_keymap_layer_to(int layer, bool activate) {
    CHECK(layer == 0 && activate);
    mouse_layer = false;
    memset(layer_refs, 0, sizeof(layer_refs));
    return 0;
}
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event, bool pressed) {
    if (strcmp(binding->behavior_dev, "layer") == 0) {
        CHECK(binding->param1 < 16);
        layer_refs[binding->param1] += pressed ? 1 : -1;
        CHECK(layer_refs[binding->param1] >= 0 && layer_refs[binding->param1] <= 1);
        if (pressed) hold_press_count++; else hold_release_count++;
        return 0;
    }
    if (strcmp(binding->behavior_dev, "autobase") == 0) {
        if (pressed) tap_press_count++; else tap_release_count++;
        struct zmk_behavior_binding copy = *binding;
        return pressed ? auto_base_pressed(&copy, event) : auto_base_released(&copy, event);
    }
    CHECK(strcmp(binding->behavior_dev, "key") == 0);
    if (pressed) tap_press_count++; else tap_release_count++;
    int err = raise_zmk_keycode_state_changed_from_encoded(binding->param1, pressed, event.timestamp);
    if (pressed && fail_tap_press) { fail_tap_press = false; return -EIO; }
    return err;
}
static int keymap_event(struct zmk_position_state_changed *event) {
    CHECK(event->position < 38 && event->source < 2);
    CHECK(delivery_count < 4096);
    deliveries[delivery_count++] = (struct delivery){event->position, event->source, event->state, event->timestamp};
    uint32_t position = event->position;
    if (event->state) {
        CHECK(!physical_bindings[position]);
        physical_bindings[position] = true;
        pressed_mapping[position] = mapping[position];
        if (position == 7 && mouse_layer && mapping[position].behavior_dev == NULL)
            pressed_mapping[position].behavior_dev = "mouse";
    } else {
        CHECK(physical_bindings[position]);
        physical_bindings[position] = false;
    }
    struct zmk_behavior_binding binding = pressed_mapping[position];
    struct zmk_behavior_binding_event bev = {.position = position, .source = event->source, .timestamp = event->timestamp};
    if (binding.behavior_dev != NULL && strncmp(binding.behavior_dev, "ht", 2) == 0)
        return event->state ? on_hold_tap_binding_pressed(&binding, bev) : on_hold_tap_binding_released(&binding, bev);
    if (binding.behavior_dev != NULL && strcmp(binding.behavior_dev, "mouse") == 0) {
        mouse_refs += event->state ? 1 : -1;
        CHECK(mouse_refs >= 0 && mouse_refs <= 1);
        return 0;
    }
    uint32_t keycode = position == 30 ? 224 : 40 + position;
    return raise_zmk_keycode_state_changed_from_encoded(keycode, event->state, event->timestamp);
}
static int zmk_event_manager_raise_at(zmk_event_t *event, const struct zmk_listener *listener) {
    CHECK(listener == &zmk_listener_behavior_hold_tap);
    CHECK(++dispatch_depth < 128);
    if (dispatch_depth > maximum_dispatch_depth) maximum_dispatch_depth = dispatch_depth;
    int result = listener->callback(event);
    if (result == ZMK_EV_EVENT_BUBBLE) {
        struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
        if (position != NULL) {
            uint32_t original_position = position->position;
            result = keymap_event(position);
            if (overwrite_replay_slot) {
                /* An adversarial reentrant observer writes the newly vacant
                 * capture slot before later subscribers read this event. */
                overwrite_replay_slot = false;
                struct captured_event replacement = {
                    .tag = ET_POS_CHANGED,
                    .data.position = {
                        .header = {.event = &zmk_event_zmk_position_state_changed},
                        .data = {.source = 0, .position = 6, .state = true, .timestamp = now}}};
                CHECK(capture_event(&replacement) == 0);
                CHECK(position->position == original_position);
            }
        }
        else {
            struct zmk_keycode_state_changed *code = as_zmk_keycode_state_changed(event);
            CHECK(code != NULL && code->keycode < 256 && code_count < 4096);
            codes[code_count++] = (struct code_delivery){code->keycode, code->state};
            code_refs[code->keycode] += code->state ? 1 : -1;
            CHECK(code_refs[code->keycode] >= 0);
        }
    }
    dispatch_depth--;
    return 0;
}
static int raise_zmk_position_state_changed(struct zmk_position_state_changed data) {
    struct zmk_position_state_changed_event event = {.header = {.event = &zmk_event_zmk_position_state_changed}, .data = data};
    return zmk_event_manager_raise_at(&event.header, &zmk_listener_behavior_hold_tap);
}
static int raise_zmk_keycode_state_changed_from_encoded(uint32_t keycode, bool pressed, int64_t timestamp) {
    struct zmk_keycode_state_changed_event event = {
        .header = {.event = &zmk_event_zmk_keycode_state_changed},
        .data = {.usage_page = 7, .keycode = keycode, .state = pressed, .timestamp = timestamp}};
    return zmk_event_manager_raise_at(&event.header, &zmk_listener_behavior_hold_tap);
}
static void reset_fixture(const char *name) {
    scenario = name; scenario_count++;
    now = 1000;
    memset(active_hold_taps, 0, sizeof(active_hold_taps));
    for (size_t i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        active_hold_taps[i].position = ZMK_BHV_HOLD_TAP_POSITION_NOT_USED;
        k_work_init_delayable(&active_hold_taps[i].work, behavior_hold_tap_timer_work_handler);
    }
    memset(captured_events, 0, sizeof(captured_events));
    undecided_hold_tap = NULL;
    last_tapped = (struct last_tapped){INT32_MIN, INT32_MIN};
    memset(mapping, 0, sizeof(mapping)); memset(pressed_mapping, 0, sizeof(pressed_mapping));
    memset(physical_bindings, 0, sizeof(physical_bindings));
    memset(layer_refs, 0, sizeof(layer_refs)); memset(code_refs, 0, sizeof(code_refs));
    memset(key_pos_states, 0, sizeof(key_pos_states));
    mouse_layer = false; mouse_refs = 0;
    sleep_calls = overflow_count = dispatch_depth = maximum_dispatch_depth = 0;
    overwrite_replay_slot = fail_tap_press = cancel_in_progress = false;
    hold_press_count = hold_release_count = tap_press_count = tap_release_count = 0;
    delivery_count = code_count = 0;
    CHECK(rx_locks == 0 && !state.rx_lock.held);
    rx_record_reset(&state); rx_corrupt_count = 0; active_transport = &esb_central;
    for (unsigned i = 0; i < 16; i++) {
        snprintf(device_names[i], sizeof(device_names[i]), "ht%u", i);
        configurations[i] = (struct behavior_hold_tap_config){
            .tapping_term_ms = 200, .flavor = FLAVOR_TAP_PREFERRED,
            .hold_behavior_dev = "layer", .tap_behavior_dev = "key"};
        devices[i].config = &configurations[i];
    }
}
static void bind_ht(unsigned position, unsigned config, bool hwu, bool auto_base) {
    CHECK(position < 38 && config < 16);
    configurations[config].hold_while_undecided = hwu;
    configurations[config].tap_behavior_dev = auto_base ? "autobase" : "key";
    mapping[position] = (struct zmk_behavior_binding){device_names[config], config + 1, 100 + position};
}
static void send_key(uint32_t position, bool pressed, int64_t timestamp) {
    now = timestamp;
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = position < 19 ? 0 : 1, .position = position, .state = pressed, .timestamp = timestamp});
}
static void wire_key(uint8_t source, uint32_t position, bool pressed, int64_t timestamp) {
    now = timestamp;
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed}};
    dispatch_wire_zmk_event(source, &event, timestamp);
}
static void enqueue_timed_key(uint8_t source, uint32_t position, bool pressed, int64_t ingress) {
    struct esb_rx_record record = {.received_at = (uint32_t)ingress, .pipe = source + 1, .length = 1};
    CHECK(source < 2 && position < 64 && timed_rx.size + sizeof(record) + 1 <= sizeof(timed_rx.data));
    memcpy(timed_rx.data + timed_rx.size, &record, sizeof(record)); timed_rx.size += sizeof(record);
    timed_rx.data[timed_rx.size++] = (uint8_t)position | (pressed ? 0x80U : 0U);
    state.rx_pipe_bytes[record.pipe] += sizeof(record) + 1;
}
static void drain_timed_keys(unsigned int budget) {
    while (budget-- && !ring_buf_is_empty(&timed_rx)) {
        struct esb_rx_record record;
        CHECK(rx_record_peek_valid(&state, &record));
        uint8_t encoded = timed_rx.data[sizeof(record)];
        size_t consumed = sizeof(record) + record.length;
        timed_rx.size -= consumed; memmove(timed_rx.data, timed_rx.data + consumed, timed_rx.size);
        state.rx_pipe_bytes[record.pipe] -= consumed;
        if (encoded == 0xff) continue; /* Rejected RF authentication/body. */
        struct zmk_split_transport_peripheral_event event = {
            .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
            .data.key_position_event = {.position = encoded & 0x3f, .pressed = (encoded & 0x80) != 0}};
        dispatch_wire_zmk_event(record.pipe - 1, &event, rx_record_timestamp(record.received_at));
    }
}
static size_t buffered(void) {
    size_t count = 0;
    for (size_t i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) count += captured_events[i].tag != ET_NONE;
    return count;
}
static void fire_timeout(uint32_t position, int64_t timestamp) {
    now = timestamp;
    struct active_hold_tap *hold_tap = find_hold_tap(position);
    CHECK(hold_tap != NULL && hold_tap->work.pending);
    hold_tap->work.pending = false;
    hold_tap->work.work.running = true;
    behavior_hold_tap_timer_work_handler(&hold_tap->work.work);
    hold_tap->work.work.running = false;
}
static void expect_idle(void) {
    CHECK(undecided_hold_tap == NULL && buffered() == 0 && mouse_refs == 0);
    CHECK(sleep_calls == 0 && dispatch_depth == 0 && maximum_dispatch_depth < 64);
    for (size_t i = 0; i < 16; i++) CHECK(layer_refs[i] == 0);
    for (size_t i = 0; i < 256; i++) CHECK(code_refs[i] == 0);
    for (size_t i = 0; i < 38; i++) CHECK(!physical_bindings[i]);
    for (size_t i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) CHECK(active_hold_taps[i].position == ZMK_BHV_HOLD_TAP_POSITION_NOT_USED);
}

static void ingress_timing_scenarios(void) {
    reset_fixture("delayed_release_keeps_ingress_tap_interval");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(0, 2, false, 1080); now = 1250; drain_timed_keys(8);
    CHECK(hold_press_count == 0 && tap_press_count == 1 && tap_release_count == 1);
    expect_idle();

    reset_fixture("timer_first_yields_to_already_queued_short_release");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(0, 2, false, 1080); fire_timeout(2, 1250);
    CHECK(hold_press_count == 0 && find_hold_tap(2)->work.pending);
    CHECK(find_hold_tap(2)->work.due == now); drain_timed_keys(8);
    CHECK(tap_press_count == 1 && tap_release_count == 1); expect_idle();

    reset_fixture("expired_queued_press_uses_immediate_nonnegative_timer");
    bind_ht(2, 0, false, false);
    enqueue_timed_key(0, 2, true, 1000); enqueue_timed_key(0, 2, false, 1080);
    now = 1500; drain_timed_keys(1);
    CHECK(find_hold_tap(2)->work.due == 1500); fire_timeout(2, 1500);
    CHECK(hold_press_count == 0); drain_timed_keys(8);
    CHECK(tap_press_count == 1); expect_idle();

    reset_fixture("release_beyond_one_rx_batch_precedes_timer");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    for (unsigned i = 0; i < 4; i++) {
        enqueue_timed_key(1, 20 + i, true, 1010 + i * 10);
        enqueue_timed_key(1, 20 + i, false, 1011 + i * 10);
    }
    enqueue_timed_key(0, 2, false, 1080); fire_timeout(2, 1250);
    drain_timed_keys(8); CHECK(!ring_buf_is_empty(&timed_rx));
    fire_timeout(2, 1251); CHECK(hold_press_count == 0); drain_timed_keys(8);
    CHECK(tap_press_count == 1); expect_idle();

    reset_fixture("newer_rx_traffic_cannot_extend_genuine_hold");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(1, 20, true, 1100); enqueue_timed_key(1, 20, false, 1110);
    enqueue_timed_key(1, 21, true, 1201); enqueue_timed_key(1, 21, false, 1202);
    fire_timeout(2, 1250); CHECK(hold_press_count == 0); drain_timed_keys(2);
    fire_timeout(2, 1251); CHECK(hold_press_count == 1 && undecided_hold_tap == NULL);
    drain_timed_keys(8); wire_key(0, 2, false, 1300); CHECK(tap_press_count == 0); expect_idle();

    reset_fixture("rx_timestamp_wrap_keeps_tap_and_deadline_order");
    bind_ht(2, 0, false, false); int64_t press_at = (int64_t)UINT32_MAX - 100;
    wire_key(0, 2, true, press_at); enqueue_timed_key(0, 2, false, press_at + 150);
    fire_timeout(2, press_at + 400); CHECK(hold_press_count == 0); drain_timed_keys(8);
    CHECK(tap_press_count == 1); expect_idle();

    reset_fixture("malformed_rx_metadata_does_not_stall_hold_timer");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(0, 2, false, 1080);
    timed_rx.data[offsetof(struct esb_rx_record, length)] = 0;
    fire_timeout(2, 1250);
    CHECK(rx_corrupt_count == 1 && hold_press_count == 1 && ring_buf_is_empty(&timed_rx));
    wire_key(0, 2, false, 1300); expect_idle();

    reset_fixture("rejected_old_packet_defers_only_until_its_dequeue");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(0, 2, false, 1080); timed_rx.data[sizeof(struct esb_rx_record)] = 0xff;
    fire_timeout(2, 1250); CHECK(hold_press_count == 0); drain_timed_keys(8);
    fire_timeout(2, 1251); CHECK(hold_press_count == 1);
    wire_key(0, 2, false, 1300); expect_idle();

    reset_fixture("inactive_esb_does_not_defer_other_transport_timer");
    bind_ht(2, 0, false, false); wire_key(0, 2, true, 1000);
    enqueue_timed_key(0, 2, false, 1080); active_transport = NULL;
    fire_timeout(2, 1250); CHECK(hold_press_count == 1);
    wire_key(0, 2, false, 1300); expect_idle();
}

int main(void) {
    reset_fixture("normal_tap_preferred_autobase_roll");
    bind_ht(2, 0, true, true); mouse_layer = true;
    send_key(2, true, 1000); CHECK(layer_refs[1] == 1);
    send_key(7, true, 1030); CHECK(buffered() == 1 && code_count == 0);
    send_key(2, false, 1050);
    CHECK(!mouse_layer && hold_press_count == 1 && hold_release_count == 1);
    CHECK(codes[0].keycode == 102 && codes[0].state && codes[1].keycode == 102 && !codes[1].state);
    CHECK(code_refs[47] == 1 && mouse_refs == 0);
    CHECK(deliveries[2].position == 7 && deliveries[2].timestamp == 1030);
    send_key(7, false, 1060); expect_idle(); CHECK(overflow_count == 0);

    reset_fixture("normal_hold_timer_then_release");
    bind_ht(2, 0, true, true); mouse_layer = true;
    send_key(2, true, 1000); fire_timeout(2, 1200);
    send_key(7, true, 1210); CHECK(mouse_refs == 1 && code_count == 0);
    send_key(7, false, 1220); send_key(2, false, 1230);
    CHECK(mouse_layer && hold_press_count == 1 && hold_release_count == 1); expect_idle();

    reset_fixture("hold_preferred_interrupt_keeps_hwu_single_press");
    bind_ht(2, 0, true, true); configurations[0].flavor = FLAVOR_HOLD_PREFERRED; mouse_layer = true;
    send_key(2, true, 1000); send_key(7, true, 1010);
    CHECK(mouse_refs == 1 && hold_press_count == 1 && undecided_hold_tap == NULL);
    send_key(7, false, 1020); send_key(2, false, 1030); expect_idle();

    reset_fixture("normal_quick_tap_keeps_held_repeat_as_tap");
    bind_ht(2, 0, false, false); configurations[0].quick_tap_ms = 125;
    send_key(2, true, 1000); send_key(2, false, 1010);
    CHECK(hold_press_count == 0 && hold_release_count == 0);
    send_key(2, true, 1060); CHECK(undecided_hold_tap == NULL && code_refs[102] == 1 && hold_press_count == 0);
    send_key(2, false, 1400); expect_idle();

    reset_fixture("capture_exactly_40_has_no_overflow");
    bind_ht(0, 0, false, false); send_key(0, true, 1000);
    for (int i = 0; i < 20; i++) { send_key(5, true, 1001 + 2*i); send_key(5, false, 1002 + 2*i); }
    CHECK(buffered() == 40 && overflow_count == 0 && delivery_count == 1);
    send_key(0, false, 1080); CHECK(delivery_count == 42 && code_count == 42); expect_idle();

    reset_fixture("capture_41_preserves_current_edge_once");
    bind_ht(0, 0, false, false); send_key(0, true, 1000);
    for (int i = 0; i < 20; i++) { send_key(5, true, 1001 + 2*i); send_key(5, false, 1002 + 2*i); }
    send_key(5, true, 1060);
    CHECK(overflow_count == 1 && delivery_count == 42 && code_refs[45] == 1 && code_refs[100] == 1);
    CHECK(deliveries[41].position == 5 && deliveries[41].timestamp == 1060);
    send_key(5, false, 1070); send_key(0, false, 1080); expect_idle();

    reset_fixture("nested_replay_without_sleep_or_overtaking");
    bind_ht(0, 0, false, false); bind_ht(1, 1, false, false);
    send_key(0, true, 1000); send_key(1, true, 1001);
    for (int i = 0; i < 19; i++) { send_key(5, true, 1002 + 2*i); send_key(5, false, 1003 + 2*i); }
    send_key(6, true, 1040); CHECK(buffered() == 40);
    send_key(6, false, 1041);
    CHECK(overflow_count == 1 && undecided_hold_tap != NULL && undecided_hold_tap->position == 1);
    CHECK(buffered() == 40 && sleep_calls == 0 && code_refs[45] == 0);
    send_key(0, false, 1060); send_key(1, false, 1070);
    CHECK(delivery_count == 44 && code_count == 44); expect_idle();

    reset_fixture("preheld_modifier_release_at_capture_capacity");
    send_key(30, true, 990); CHECK(code_refs[224] == 1);
    bind_ht(0, 0, false, false); send_key(0, true, 1000);
    for (int i = 0; i < 20; i++) { send_key(5, true, 1001 + 2*i); send_key(5, false, 1002 + 2*i); }
    send_key(30, false, 1050);
    CHECK(overflow_count == 1 && code_refs[224] == 0 && codes[code_count - 1].keycode == 224 && !codes[code_count - 1].state);
    send_key(0, false, 1060); expect_idle();

    reset_fixture("hwu_overflow_releases_layer_once_before_autobase");
    bind_ht(2, 0, true, true); mouse_layer = true;
    send_key(2, true, 1000);
    for (int i = 0; i < 20; i++) { send_key(7, true, 1001 + 2*i); send_key(7, false, 1002 + 2*i); }
    send_key(7, true, 1050);
    CHECK(overflow_count == 1 && !mouse_layer && mouse_refs == 0 && code_refs[47] == 1);
    CHECK(hold_press_count == 1 && hold_release_count == 1);
    send_key(7, false, 1060); send_key(2, false, 1070); expect_idle();

    reset_fixture("held_slot_11_emits_full_tap_and_cannot_release_reused_slot");
    for (unsigned i = 0; i < 11; i++) bind_ht(i, i, false, false);
    for (unsigned i = 0; i < 10; i++) { send_key(i, true, 1000 + 300*i); fire_timeout(i, 1200 + 300*i); }
    send_key(10, true, 4100);
    CHECK(overflow_count == 1 && find_hold_tap(10) == NULL && code_refs[110] == 0);
    CHECK(codes[0].keycode == 110 && codes[0].state && codes[1].keycode == 110 && !codes[1].state);
    send_key(0, false, 4110); bind_ht(11, 11, false, false); send_key(11, true, 4120); fire_timeout(11, 4320);
    send_key(10, false, 4330); CHECK(find_hold_tap(11) != NULL && layer_refs[12] == 1);
    for (unsigned i = 1; i < 10; i++) send_key(i, false, 4340 + i);
    send_key(11, false, 4350); expect_idle();

    reset_fixture("held_slot_fallback_attempts_release_after_child_press_error");
    for (unsigned i = 0; i < 11; i++) bind_ht(i, i, false, false);
    for (unsigned i = 0; i < 10; i++) { send_key(i, true, 1000 + 300*i); fire_timeout(i, 1200 + 300*i); }
    CHECK(find_hold_tap(10) == NULL); fail_tap_press = true;
    send_key(10, true, 4100);
    CHECK(!fail_tap_press && overflow_count == 1 && code_refs[110] == 0 && code_count == 2);
    send_key(10, false, 4110);
    for (unsigned i = 0; i < 10; i++) send_key(i, false, 4200 + i);
    expect_idle();

    reset_fixture("central_bitmap_commits_before_capture_snapshot_replay");
    bind_ht(2, 0, true, true); mouse_layer = true;
    wire_key(0, 2, true, 1000); wire_key(1, 7, true, 1010);
    CHECK(totem_esb_key_state_get(key_pos_states[1], 7) && buffered() == 1 && mouse_refs == 0);
    uint8_t empty[5] = {0}, source = 1;
    now = 1020; CHECK(totem_esb_key_state_reconcile(key_pos_states[1], empty, 38, emit_snapshot_key, &source) == 1);
    CHECK(!totem_esb_key_state_get(key_pos_states[1], 7) && buffered() == 2);
    wire_key(1, 7, false, 1030); CHECK(buffered() == 2);
    wire_key(0, 2, false, 1040);
    CHECK(code_count == 4 && codes[2].keycode == 47 && codes[2].state && codes[3].keycode == 47 && !codes[3].state);
    CHECK(deliveries[2].source == 1 && deliveries[2].timestamp == 1010 && deliveries[3].timestamp == 1020);
    expect_idle();

    reset_fixture("three_nested_hold_taps_keep_original_timestamps");
    bind_ht(0, 0, false, false); bind_ht(1, 1, false, false); bind_ht(2, 2, false, false);
    send_key(0, true, 1000); send_key(1, true, 1001); send_key(2, true, 1002);
    send_key(5, true, 1003); send_key(5, false, 1004);
    send_key(2, false, 1005); send_key(1, false, 1006); send_key(0, false, 1007);
    CHECK(delivery_count == 8 && code_count == 8 && hold_press_count == 0 && overflow_count == 0);
    expect_idle();

    reset_fixture("overflow_retries_own_release_of_new_undecided_hold_tap");
    bind_ht(0, 0, false, false); bind_ht(1, 1, false, false);
    send_key(0, true, 1000); send_key(1, true, 1001);
    for (int i = 0; i < 19; i++) { send_key(5, true, 1002 + 2*i); send_key(5, false, 1003 + 2*i); }
    send_key(6, true, 1040); CHECK(buffered() == 40);
    send_key(1, false, 1041);
    CHECK(overflow_count == 1 && undecided_hold_tap == NULL && buffered() == 0 && code_refs[46] == 1);
    send_key(6, false, 1042); send_key(0, false, 1043); expect_idle();

    reset_fixture("queued_timer_cleanup_remains_deferred");
    bind_ht(0, 0, false, false); send_key(0, true, 1000);
    cancel_in_progress = true; send_key(0, false, 1010);
    struct active_hold_tap *cancelled = find_hold_tap(0);
    CHECK(cancelled != NULL && cancelled->work_is_cancelled && code_refs[100] == 0);
    cancelled->work.pending = false;
    behavior_hold_tap_timer_work_handler(&cancelled->work.work);
    expect_idle();

    reset_fixture("replay_local_copy_survives_reentrant_slot_reuse");
    struct captured_event replay = {
        .tag = ET_POS_CHANGED,
        .data.position = {
            .header = {.event = &zmk_event_zmk_position_state_changed},
            .data = {.source = 0, .position = 5, .state = true, .timestamp = 1000}}};
    CHECK(capture_event(&replay) == 0);
    overwrite_replay_slot = true;
    release_captured_events();
    CHECK(!overwrite_replay_slot && buffered() == 1 && deliveries[0].position == 5);
    release_captured_events();
    CHECK(buffered() == 0 && deliveries[1].position == 6);
    send_key(5, false, 1010); send_key(6, false, 1020); expect_idle();

    reset_fixture("maximum_10_nested_hold_taps_complete_with_bounded_reentry");
    for (unsigned i = 0; i < 10; i++) bind_ht(i, i, false, false);
    for (unsigned i = 0; i < 10; i++) send_key(i, true, 1000 + i);
    for (unsigned i = 10; i > 0; i--) send_key(i - 1, false, 1020 - i);
    CHECK(delivery_count == 20 && code_count == 20 && overflow_count == 0 && hold_press_count == 0);
    expect_idle();

    ingress_timing_scenarios();
    printf("%u actual hold-tap scenarios passed\n", scenario_count);
    return 0;
}
