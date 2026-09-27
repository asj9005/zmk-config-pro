/* Deterministic ZMK event/keymap fakes for the actual Auto Base callbacks. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Report regressions without opening the Windows CRT assertion dialog. */
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Auto Base assertion failed: %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define ZMK_BEHAVIOR_OPAQUE 0
#define LOG_ERR(...) ((void)0)
#define BIT(layer) (1U << (layer))
#define LAYER_COUNT 4U

struct zmk_behavior_binding {
    const char *behavior_dev;
    uint32_t param1;
    uint32_t param2;
};

struct zmk_behavior_binding_event {
    int layer;
    uint32_t position;
    int64_t timestamp;
    uint8_t source;
};

enum action_type { KEY_DOWN, KEY_UP, LAYER_TO };
struct action {
    enum action_type type;
    uint32_t encoded;
    int64_t timestamp;
    uint8_t layer;
    bool locking;
};

static struct action actions[16];
static unsigned action_count;
static uint32_t held_key;
static uint8_t implicit_modifiers;
static uint8_t physical_modifiers;
static uint8_t active_layer;
static uint32_t layer_state, layer_locks;
static unsigned int mouse_events;
static unsigned pending_macro_actions;
static int press_result, release_result, layer_result;

static int raise_zmk_keycode_state_changed_from_encoded(uint32_t encoded, bool pressed,
                                                       int64_t timestamp) {
    assert(action_count < sizeof(actions) / sizeof(actions[0]));
    actions[action_count++] = (struct action){.type = pressed ? KEY_DOWN : KEY_UP,
                                              .encoded = encoded,
                                              .timestamp = timestamp};
    /* Model an event listener which can report an error after changing state. */
    held_key = pressed ? encoded : 0;
    implicit_modifiers = pressed ? (uint8_t)(encoded >> 24) : 0;
    return pressed ? press_result : release_result;
}

/* Match the layer state/lock rules in pinned ZMK keymap.c set_layer_state():
 * https://github.com/zmkfirmware/zmk/blob/904c9aec8822d79149d42c8a9a77e8828eb08f5a/app/src/keymap.c
 * Base stays active, non-locking calls cannot disable a locked layer, and
 * locking calls update the lock bit along with the active bit. Event delivery
 * and layer ordering are outside this fixture; the stock order is 0..3.
 */
static void set_layer_state(uint8_t layer, bool state, bool locking) {
    assert(layer < LAYER_COUNT);
    if ((layer == 0U && !state) || (!locking && !state && (layer_locks & BIT(layer)))) {
        return;
    }
    if (state) {
        layer_state |= BIT(layer);
    } else {
        layer_state &= ~BIT(layer);
    }
    if (locking) {
        if (state) {
            layer_locks |= BIT(layer);
        } else {
            layer_locks &= ~BIT(layer);
        }
    }
}

static int zmk_keymap_layer_to(uint8_t layer, bool locking) {
    assert(action_count < sizeof(actions) / sizeof(actions[0]));
    actions[action_count++] =
        (struct action){.type = LAYER_TO, .layer = layer, .locking = locking};
    if (layer_result >= 0) {
        for (unsigned int i = LAYER_COUNT; i > 0U; --i) {
            set_layer_state((uint8_t)(i - 1U), false, locking);
        }
        set_layer_state(layer, true, locking);
        active_layer = 0;
        for (uint8_t i = 0; i < LAYER_COUNT; ++i) {
            if (layer_state & BIT(i)) {
                active_layer = i;
            }
        }
    }
    return layer_result;
}

/* ACTUAL_FIRMWARE_FUNCTIONS */

static const struct zmk_behavior_binding_event event = {
    .layer = 1, .position = 2, .timestamp = 12345, .source = 1};

static void reset_state(void) {
    memset(actions, 0, sizeof(actions));
    action_count = 0;
    held_key = 0;
    implicit_modifiers = 0;
    physical_modifiers = 0x08;
    active_layer = 1;
    layer_state = BIT(0) | BIT(1);
    layer_locks = 0;
    mouse_events = 0;
    pending_macro_actions = 0;
    press_result = release_result = layer_result = 0;
}

static void assert_tap_then_base(uint32_t encoded) {
    assert(action_count == 3);
    assert(actions[0].type == KEY_DOWN);
    assert(actions[1].type == KEY_UP);
    assert(actions[0].encoded == encoded && actions[1].encoded == encoded);
    assert(actions[0].timestamp == event.timestamp);
    assert(actions[1].timestamp == event.timestamp);
    assert(actions[2].type == LAYER_TO);
    assert(actions[2].layer == 0 && actions[2].locking);
    assert(held_key == 0);
    assert(implicit_modifiers == 0);
    assert(physical_modifiers == 0x08);
}

static void test_tap_and_release(void) {
    reset_state();
    struct zmk_behavior_binding binding = {.param1 = 0x00070008}; /* E */
    assert(auto_base_pressed(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert_tap_then_base(binding.param1);
    assert(active_layer == 0);
    assert(auto_base_released(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert(auto_base_released(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert_tap_then_base(binding.param1); /* Physical release never repeats the tap. */
}

static void test_busy_macro_queue(void) {
    reset_state();
    pending_macro_actions = 12; /* Another macro is waiting on its delayed work. */
    struct zmk_behavior_binding binding = {.param1 = 0x00070015}; /* R */
    assert(auto_base_pressed(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert_tap_then_base(binding.param1);
    assert(pending_macro_actions == 12);
    /* A hold-tap replays captured I immediately after this callback returns. */
    assert(active_layer == 0);
    assert(auto_base_released(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert(action_count == 3 && pending_macro_actions == 12);
}

static void test_modified_key_and_usage_page(void) {
    const uint32_t encoded_keys[] = {0x07070013, 0x000c00e9};
    for (unsigned i = 0; i < sizeof(encoded_keys) / sizeof(encoded_keys[0]); ++i) {
        reset_state();
        struct zmk_behavior_binding binding = {.param1 = encoded_keys[i]};
        assert(auto_base_pressed(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
        assert_tap_then_base(binding.param1);
        assert(active_layer == 0);
    }
}

static void test_press_error_still_releases_and_returns_to_base(void) {
    reset_state();
    press_result = -ENOSPC;
    struct zmk_behavior_binding binding = {.param1 = 0x03070008};
    assert(auto_base_pressed(&binding, event) == -ENOSPC);
    assert_tap_then_base(binding.param1);
    assert(active_layer == 0);
}

static void test_release_error_still_returns_to_base(void) {
    reset_state();
    release_result = -EIO;
    struct zmk_behavior_binding binding = {.param1 = 0x00070008};
    assert(auto_base_pressed(&binding, event) == -EIO);
    assert_tap_then_base(binding.param1);
    assert(active_layer == 0);
}

static void test_layer_error_is_propagated(void) {
    reset_state();
    layer_result = -EINVAL;
    struct zmk_behavior_binding binding = {.param1 = 0x00070008};
    assert(auto_base_pressed(&binding, event) == -EINVAL);
    assert_tap_then_base(binding.param1);
    assert(active_layer == 1); /* Failed transitions are not reported as success. */
}

static void test_first_error_is_preserved(void) {
    reset_state();
    press_result = -ENOSPC;
    release_result = -EIO;
    layer_result = -EINVAL;
    struct zmk_behavior_binding binding = {.param1 = 0x00070008};
    assert(auto_base_pressed(&binding, event) == -ENOSPC);
    assert_tap_then_base(binding.param1);
}

static void test_positive_event_results_do_not_fall_through(void) {
    reset_state();
    press_result = release_result = 1;
    struct zmk_behavior_binding binding = {.param1 = 0x00070008};
    assert(auto_base_pressed(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert_tap_then_base(binding.param1);
    assert(active_layer == 0);
}

/* The captured I position is resolved only after the tap callback returns.
 * The fixture needs only its two relevant bindings: Base => I, Mouse => move.
 */
static void replay_following_i(void) {
    if (active_layer == 0U) {
        (void)raise_zmk_keycode_state_changed_from_encoded(0x0007000c, true, event.timestamp + 1);
        (void)raise_zmk_keycode_state_changed_from_encoded(0x0007000c, false, event.timestamp + 1);
    } else {
        mouse_events++;
    }
}

static void test_locked_mouse_returns_to_base_before_following_i(void) {
    reset_state();
    /* Stock &to has locking=true, including entry through a hold-tap's &to. */
    assert(zmk_keymap_layer_to(1, true) == 0);
    assert(active_layer == 1 && (layer_locks & BIT(1)) != 0U);
    action_count = 0; /* Keep the actual locked state established above. */

    struct zmk_behavior_binding binding = {.param1 = 0x00070008}; /* E tap */
    assert(auto_base_pressed(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    assert(auto_base_released(&binding, event) == ZMK_BEHAVIOR_OPAQUE);
    replay_following_i();

    /* The old layer_to(0, false) leaves Mouse locked and fails right here. */
    assert(mouse_events == 0U);
    assert(active_layer == 0 && (layer_locks & BIT(1)) == 0U);
    assert(layer_state == BIT(0));
    assert(action_count == 5);
    assert(actions[0].type == KEY_DOWN && actions[0].encoded == binding.param1);
    assert(actions[1].type == KEY_UP && actions[1].encoded == binding.param1);
    assert(actions[2].type == LAYER_TO && actions[2].layer == 0 && actions[2].locking);
    assert(actions[3].type == KEY_DOWN && actions[3].encoded == 0x0007000c);
    assert(actions[4].type == KEY_UP && actions[4].encoded == 0x0007000c);
    assert(held_key == 0 && physical_modifiers == 0x08);
}

int main(void) {
    test_locked_mouse_returns_to_base_before_following_i();
    test_tap_and_release();
    test_busy_macro_queue();
    test_modified_key_and_usage_page();
    test_press_error_still_releases_and_returns_to_base();
    test_release_error_still_returns_to_base();
    test_layer_error_is_propagated();
    test_first_error_is_preserved();
    test_positive_event_results_do_not_fall_through();
    puts("9 Auto Base callback cases passed");
    return 0;
}
