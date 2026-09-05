/* Deterministic ZMK event/keymap fakes for the actual Auto Base callbacks. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ZMK_BEHAVIOR_OPAQUE 0
#define LOG_ERR(...) ((void)0)

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

static int zmk_keymap_layer_to(uint8_t layer, bool locking) {
    assert(action_count < sizeof(actions) / sizeof(actions[0]));
    actions[action_count++] =
        (struct action){.type = LAYER_TO, .layer = layer, .locking = locking};
    if (layer_result >= 0) {
        active_layer = layer;
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
    assert(actions[2].layer == 0 && !actions[2].locking);
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

int main(void) {
    test_tap_and_release();
    test_busy_macro_queue();
    test_modified_key_and_usage_page();
    test_press_error_still_releases_and_returns_to_base();
    test_release_error_still_returns_to_base();
    test_layer_error_is_propagated();
    test_first_error_is_preserved();
    test_positive_event_results_do_not_fall_through();
    puts("8 Auto Base callback cases passed");
    return 0;
}
