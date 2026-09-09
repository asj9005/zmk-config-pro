/* SPDX-License-Identifier: MIT */
/* Deterministic Zephyr boundary fakes around unmodified production mouse C.
 * Layer getters expose pinned ZMK 904c9aec's state/default public API values.
 * No keymap/hold-tap replay, input-thread scheduling, or HID device is modeled.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <totem/pointing_curve.h>
#include "mouse-tuning.h"

static const char *case_name;
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", case_name, __LINE__, #test); exit(1); \
} } while (0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define IS_ENABLED(value) (value)
#define CONFIG_SYS_CLOCK_TICKS_PER_SEC 1000
#define K_MSEC(value) (value)
#define K_NO_WAIT 0
#define INPUT_REL_X 0
#define INPUT_REL_Y 1
#define INPUT_REL_HWHEEL 6
#define INPUT_REL_WHEEL 8
static unsigned bit_calls;
static uint32_t checked_bit(int n) {
    CHECK(n >= 0 && n < 32);
    bit_calls++;
    return UINT32_C(1) << n;
}
#define BIT(n) checked_bit(n)
/* The decode operations are verbatim from the pinned pointing binding header.
 * Use unsigned fixture packing to avoid signed host shifts for negative X. */
#define MOVE_Y_DECODE(encoded) (int16_t)((encoded) & 0x0000FFFF)
#define MOVE_X_DECODE(encoded) (int16_t)(((encoded) & 0xFFFF0000) >> 16)

static void fake_log(const char *format, ...) { (void)format; }
#define LOG_DBG(...) fake_log(__VA_ARGS__)

struct k_work { int unused; };
struct k_work_delayable {
    struct k_work work;
    void (*callback)(struct k_work *);
    bool pending;
};
struct device { const void *config; void *data; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1; };
struct zmk_behavior_binding_event { uint32_t position; };
typedef uint32_t zmk_keymap_layers_state_t;
typedef uint8_t zmk_keymap_layer_id_t;
static const struct device *fake_device;
static int64_t fake_now;
static unsigned schedule_calls, cancel_calls;
static zmk_keymap_layers_state_t layer_state;
static uint8_t default_layer;
static unsigned snapshot_calls, default_calls;
static bool change_after_snapshot;

static int64_t k_uptime_ticks(void) { return fake_now; }
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static void k_work_init_delayable(struct k_work_delayable *work, void (*callback)(struct k_work *)) {
    work->callback = callback;
    work->pending = false;
}
static int k_work_schedule(struct k_work_delayable *work, int delay) {
    CHECK(delay > 0);
    schedule_calls++;
    work->pending = true;
    return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    cancel_calls++;
    work->pending = false;
    return 0;
}
static const struct device *zmk_behavior_get_binding(const char *name) {
    CHECK(strcmp(name, "mmv") == 0);
    return fake_device;
}
static zmk_keymap_layers_state_t zmk_keymap_layer_state(void) {
    snapshot_calls++;
    zmk_keymap_layers_state_t snapshot = layer_state;
    if (change_after_snapshot) {
        layer_state = BIT(3);
    }
    return snapshot;
}
static zmk_keymap_layer_id_t zmk_keymap_layer_default(void) {
    default_calls++;
    return default_layer;
}
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
struct fake_resolution_profile { uint8_t wheel; uint8_t hor_wheel; };
static struct fake_resolution_profile zmk_pointing_resolution_multipliers_get_current_profile(void) {
    return (struct fake_resolution_profile){1, 1};
}
#endif

struct recorded_input { uint16_t code; int16_t value; bool sync; };
static struct recorded_input reports[16];
static size_t report_count;
static bool change_layer_on_report;
static int input_report_rel(const struct device *dev, uint16_t code, int16_t value, bool sync,
                            int timeout) {
    CHECK(dev == fake_device && timeout == K_NO_WAIT);
    CHECK(report_count < sizeof(reports) / sizeof(reports[0]));
    reports[report_count++] = (struct recorded_input){code, value, sync};
    if (change_layer_on_report) {
        layer_state = BIT(3);
    }
    return 0;
}

/* ACTUAL_MOUSE_BEHAVIOR */

static struct behavior_input_two_axis_config cfg;
static struct behavior_input_two_axis_data data;
static struct device dev;

static void reset_case(const char *name) {
    case_name = name;
    memset(&data, 0, sizeof(data));
    cfg = (struct behavior_input_two_axis_config){
        .x_code = INPUT_REL_X, .y_code = INPUT_REL_Y,
        .trigger_period_ms = TOTEM_MOUSE_TRIGGER_PERIOD_MS,
        .time_to_max_speed_ms = TOTEM_MOUSE_TIME_TO_MAX_MS,
        .acceleration_exponent = TOTEM_MOUSE_ACCEL_EXPONENT,
        .acceleration_boost_start_ms = TOTEM_MOUSE_BOOST_START_MS,
        .acceleration_boost_numerator = TOTEM_MOUSE_BOOST_NUM,
        .acceleration_boost_denominator = TOTEM_MOUSE_BOOST_DEN,
        .fast_layer = 2, .precise_layer = 3,
        .fast_speed = (float)TOTEM_MOUSE_FAST_SPEED_NUM / TOTEM_MOUSE_FAST_SPEED_DEN,
        .precise_speed = (float)TOTEM_MOUSE_SLOW_SPEED_NUM / TOTEM_MOUSE_SLOW_SPEED_DEN,
    };
    dev = (struct device){&cfg, &data};
    fake_device = &dev;
    fake_now = 1000;
    schedule_calls = cancel_calls = snapshot_calls = default_calls = bit_calls = 0;
    layer_state = 0;
    default_layer = 0;
    change_after_snapshot = change_layer_on_report = false;
    report_count = 0;
    CHECK(behavior_input_two_axis_init(&dev) == 0);
}

static struct zmk_behavior_binding binding(int16_t x, int16_t y) {
    return (struct zmk_behavior_binding){"mmv", ((uint32_t)(uint16_t)x << 16) | (uint16_t)y};
}
static void press(struct zmk_behavior_binding *key) {
    CHECK(on_keymap_binding_pressed(key, (struct zmk_behavior_binding_event){1}) == 0);
}
static void release(struct zmk_behavior_binding *key) {
    CHECK(on_keymap_binding_released(key, (struct zmk_behavior_binding_event){1}) == 0);
}
static void tick_at(int64_t now) {
    fake_now = now;
    data.tick_work.pending = false;
    data.tick_work.callback(&data.tick_work.work);
}
static void clear_check(const struct movement_state_1d *state) {
    CHECK(state->speed == 0 && state->start_time == 0);
    for (int i = 0; i < MOUSE_SPEED_MODE_COUNT; i++) {
        CHECK(state->remainder[i] == 0);
    }
}
static void seed_remainders(struct movement_state_1d *state) {
    for (int i = 0; i < MOUSE_SPEED_MODE_COUNT; i++) {
        state->remainder[i] = (float)(i + 1) / 8;
    }
}
static void same_float(float actual, float expected) {
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
}

static void mode_selection(void) {
    reset_case("mode_selection");
    const uint32_t masks[] = {0, BIT(2), BIT(3), BIT(2) | BIT(3), BIT(8)};
    const enum mouse_speed_mode expected[] = {MOUSE_SPEED_NORMAL, MOUSE_SPEED_FAST,
        MOUSE_SPEED_PRECISE, MOUSE_SPEED_FAST, MOUSE_SPEED_NORMAL};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        layer_state = masks[i];
        snapshot_calls = default_calls = 0;
        CHECK(current_speed_mode(&cfg) == expected[i]);
        CHECK(snapshot_calls == 1 && default_calls == 1);
    }
    layer_state = 0;
    default_layer = 2;
    CHECK(current_speed_mode(&cfg) == MOUSE_SPEED_FAST);
    default_layer = 3;
    CHECK(current_speed_mode(&cfg) == MOUSE_SPEED_PRECISE);
    cfg.fast_layer = cfg.precise_layer = -1;
    default_layer = UINT8_MAX;
    bit_calls = 0;
    layer_state = UINT32_MAX;
    CHECK(current_speed_mode(&cfg) == MOUSE_SPEED_NORMAL && bit_calls == 0);
    cfg.precise_layer = 3;
    CHECK(current_speed_mode(&cfg) == MOUSE_SPEED_PRECISE);
    cfg.fast_layer = 2;
    default_layer = 0;
    layer_state = BIT(2);
    change_after_snapshot = true;
    CHECK(current_speed_mode(&cfg) == MOUSE_SPEED_FAST);
    CHECK(layer_state == BIT(3));
}

static void absolute_fixed_speeds(void) {
    reset_case("absolute_fixed_speeds");
    CHECK(cfg.fast_speed == 15750 && cfg.precise_speed == 562.5f);
    const float signs[] = {-2700, -1, 0, 1, 2700};
    const int64_t times[] = {0, 1, 16, 500, 900, 901, INT64_C(4294967295)};
    for (int variant = 0; variant < 2; variant++) {
        if (variant) {
            cfg.time_to_max_speed_ms = 0;
            cfg.acceleration_exponent = 0;
            cfg.acceleration_boost_numerator = 1;
        }
        for (size_t s = 0; s < sizeof(signs) / sizeof(signs[0]); s++) {
            for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
                float direction = signs[s] < 0 ? -1 : (signs[s] > 0 ? 1 : 0);
                CHECK(speed(&cfg, INPUT_REL_X, signs[s], times[t], MOUSE_SPEED_FAST) == direction * 15750);
                CHECK(speed(&cfg, INPUT_REL_Y, signs[s], times[t], MOUSE_SPEED_PRECISE) == direction * 562.5f);
            }
        }
    }
}

static void normal_state_isolation(void) {
    reset_case("normal_state_isolation");
    /* Fractional fixed-mode reports make a shared remainder regression visible
     * even though today's 16 ms fixed speeds produce exact 252/9 reports. */
    cfg.trigger_period_ms = 7;
    data.state.x.speed = ZMK_POINTING_DEFAULT_MOVE_VAL;
    data.state.x.start_time = fake_now;
    seed_remainders(&data.state.x);
    (void)update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1016, MOUSE_SPEED_NORMAL);
    struct movement_state_1d normal_reference = data.state.x;
    float saved_normal_remainder = data.state.x.remainder[MOUSE_SPEED_NORMAL];
    (void)update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1100, MOUSE_SPEED_FAST);
    same_float(data.state.x.remainder[MOUSE_SPEED_NORMAL], saved_normal_remainder);
    (void)update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1200, MOUSE_SPEED_PRECISE);
    same_float(data.state.x.remainder[MOUSE_SPEED_NORMAL], saved_normal_remainder);
    CHECK(data.state.x.start_time == 1000 && data.state.x.speed == ZMK_POINTING_DEFAULT_MOVE_VAL);
    float expected = update_movement_1d(&cfg, INPUT_REL_X, &normal_reference, 1350, MOUSE_SPEED_NORMAL);
    float actual = update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1350, MOUSE_SPEED_NORMAL);
    same_float(actual, expected);
    same_float(data.state.x.remainder[MOUSE_SPEED_NORMAL], normal_reference.remainder[MOUSE_SPEED_NORMAL]);
    CHECK(data.state.x.start_time == normal_reference.start_time);
    CHECK(speed(&cfg, INPUT_REL_X, 2700, 900, MOUSE_SPEED_NORMAL) == 4500);
    CHECK(fabsf(speed(&cfg, INPUT_REL_X, 2700, 200, MOUSE_SPEED_NORMAL) - 133.333333f) < 0.001f);
}

static void single_tick_snapshot(void) {
    reset_case("single_tick_snapshot");
    struct zmk_behavior_binding diagonal = binding(ZMK_POINTING_DEFAULT_MOVE_VAL, -ZMK_POINTING_DEFAULT_MOVE_VAL);
    press(&diagonal);
    layer_state = BIT(2);
    change_after_snapshot = change_layer_on_report = true;
    tick_at(1016);
    CHECK(snapshot_calls == 1 && default_calls == 1);
    CHECK(report_count == 2);
    CHECK(reports[0].code == INPUT_REL_X && reports[0].value == 252 && !reports[0].sync);
    CHECK(reports[1].code == INPUT_REL_Y && reports[1].value == -252 && reports[1].sync);
    CHECK(data.state.x.start_time == 1000 && data.state.y.start_time == 1000);
    CHECK(data.tick_work.pending);
    report_count = snapshot_calls = default_calls = 0;
    tick_at(1032);
    CHECK(snapshot_calls == 1 && default_calls == 1 && report_count == 2);
    CHECK(reports[0].value == 9 && reports[1].value == -9);
    release(&diagonal);
}

static void binding_release_after_base(void) {
    reset_case("binding_release_after_base");
    struct zmk_behavior_binding right = binding(ZMK_POINTING_DEFAULT_MOVE_VAL, 0);
    struct zmk_behavior_binding up = binding(0, -ZMK_POINTING_DEFAULT_MOVE_VAL);
    layer_state = BIT(2);
    press(&right);
    press(&up);
    seed_remainders(&data.state.x);
    seed_remainders(&data.state.y);
    /* The pinned keymap supplies the original binding on release after Base.
     * Execute that binding's actual release callback; do not emulate keymap. */
    layer_state = 0;
    release(&right);
    clear_check(&data.state.x);
    CHECK(data.state.y.speed == -ZMK_POINTING_DEFAULT_MOVE_VAL && data.state.y.start_time == 1000);
    release(&up);
    clear_check(&data.state.y);
    CHECK(!data.tick_work.pending && cancel_calls == 1);
    unsigned schedules_before = schedule_calls;
    tick_at(1100); /* A callback already in flight must not report/rearm at zero. */
    CHECK(report_count == 0 && schedule_calls == schedules_before && !data.tick_work.pending);
}

static void opposite_keys_cancel_and_resume(void) {
    reset_case("opposite_keys_cancel_and_resume");
    struct zmk_behavior_binding right = binding(ZMK_POINTING_DEFAULT_MOVE_VAL, 0);
    struct zmk_behavior_binding left = binding(-ZMK_POINTING_DEFAULT_MOVE_VAL, 0);
    press(&right);
    seed_remainders(&data.state.x);
    fake_now = 1200;
    layer_state = BIT(2);
    press(&left);
    clear_check(&data.state.x);
    CHECK(!data.tick_work.pending);
    fake_now = 1400;
    layer_state = BIT(3);
    release(&left); /* Right remains held; its net movement starts afresh. */
    CHECK(data.state.x.speed == ZMK_POINTING_DEFAULT_MOVE_VAL && data.state.x.start_time == 1400);
    for (int i = 0; i < MOUSE_SPEED_MODE_COUNT; i++) CHECK(data.state.x.remainder[i] == 0);
    tick_at(1416);
    CHECK(report_count == 1 && reports[0].value == 9);
    layer_state = 0;
    release(&right);
    clear_check(&data.state.x);
    CHECK(!data.tick_work.pending);
}

static void zero_and_delay_gates(void) {
    reset_case("zero_and_delay_gates");
    seed_remainders(&data.state.x);
    CHECK(update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1100, MOUSE_SPEED_FAST) == 0);
    clear_check(&data.state.x);
    data.state.x.speed = ZMK_POINTING_DEFAULT_MOVE_VAL;
    data.state.x.start_time = 1000;
    cfg.delay_ms = 20;
    CHECK(update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1016, MOUSE_SPEED_FAST) == 0);
    CHECK(update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1020, MOUSE_SPEED_FAST) == 0);
    CHECK(update_movement_1d(&cfg, INPUT_REL_X, &data.state.x, 1032, MOUSE_SPEED_FAST) == 252);
    CHECK(data.state.x.start_time == 1000);
}

int main(void) {
    mode_selection();
    absolute_fixed_speeds();
    normal_state_isolation();
    single_tick_snapshot();
    binding_release_after_base();
    opposite_keys_cancel_and_resume();
    zero_and_delay_gates();
    puts("7 actual mouse behavior cases passed");
    return 0;
}
