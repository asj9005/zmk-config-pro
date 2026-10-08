/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "brightness assertion failed at %d: %s\n", __LINE__, #condition); \
    exit(1); \
} } while (0)
#define ARG_UNUSED(x) ((void)(x))
#define LOG_ERR(...) ((void)0)
#define LOG_MODULE_REGISTER(...) _Static_assert(1, "fake logger")
#define printk(...) ((void)0)
#define K_THREAD_DEFINE(...) _Static_assert(1, "thread invoked by fixture")
#define SYS_INIT(...) _Static_assert(1, "init invoked by fixture")
#define CONFIG_PROSPECTOR_FIXED_BRIGHTNESS 75
#define DEVICE_DT_GET_ONE(name) (&name##_device)
#define DT_NODELABEL(name) 0
#define DT_NODE_CHILD_IDX(node) 0
struct device { bool ready; };
static struct device pwm_leds_device = {true};
static bool device_is_ready(const struct device *dev) { return dev->ready; }
static uint8_t actual_pwm = 100, applied[256];
static unsigned int pwm_calls, applied_count, fail_pwm_call;
static int led_set_brightness(const struct device *dev, int channel, uint8_t value) {
    CHECK(dev == &pwm_leds_device && dev->ready && channel == 0 && value <= 100);
    pwm_calls++;
    if (pwm_calls == fail_pwm_call) { return -EIO; }
    CHECK(applied_count < sizeof(applied));
    applied[applied_count++] = actual_pwm = value;
    return 0;
}

#if TEST_AMBIENT
#define CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR 1
#define SENSOR_CHAN_LIGHT 1
static struct device avago_apds9960_device = {true};
struct sensor_value { int32_t val1, val2; };
struct reading { int fetch_error, channel_error; int32_t value; };
static struct reading readings[16], fetched;
static unsigned int reading_count, reading_next, fetch_calls, channel_calls;
static unsigned int normal_sleeps, burst_sleeps, error_sleeps, fade_sleeps, sleep_total;
static unsigned int normal_limit;
static bool thread_running;
static jmp_buf thread_end;
static int sensor_sample_fetch(const struct device *dev) {
    CHECK(dev == &avago_apds9960_device && dev->ready);
    CHECK(reading_next < reading_count);
    fetched = readings[reading_next++]; fetch_calls++;
    return fetched.fetch_error;
}
static int sensor_channel_get(const struct device *dev, int channel, struct sensor_value *value) {
    CHECK(dev == &avago_apds9960_device && dev->ready && channel == SENSOR_CHAN_LIGHT);
    CHECK(fetched.fetch_error == 0); channel_calls++;
    /* An error may leave stale/poisoned output; callers must ignore it. */
    value->val1 = fetched.channel_error ? 0 : fetched.value;
    value->val2 = 0;
    return fetched.channel_error;
}
static void k_msleep(unsigned int ms) {
    CHECK(++sleep_total < 1000);
    if (ms == 100) {
        if (thread_running && normal_sleeps == normal_limit) { longjmp(thread_end, 1); }
        normal_sleeps++;
    } else if (ms == 30) {
        burst_sleeps++;
    } else if (ms == 1000) {
        error_sleeps++;
    } else {
        CHECK(ms == 3 || ms == 10); fade_sleeps++;
    }
}
#endif

/* ACTUAL_BRIGHTNESS_SOURCE */

static void reset_fixture(void) {
    pwm_leds_device.ready = true; actual_pwm = 100;
    pwm_calls = applied_count = fail_pwm_call = 0;
#if TEST_AMBIENT
    avago_apds9960_device.ready = true; current_brightness = 100;
    reading_count = reading_next = fetch_calls = channel_calls = 0;
    normal_sleeps = burst_sleeps = error_sleeps = fade_sleeps = sleep_total = 0;
    thread_running = false;
#endif
}

#if TEST_AMBIENT
static void add_reading(int fetch_error, int channel_error, int32_t value) {
    CHECK(reading_count < sizeof(readings) / sizeof(readings[0]));
    readings[reading_count++] = (struct reading){fetch_error, channel_error, value};
}
static void run_thread(unsigned int cycles) {
    normal_limit = cycles; thread_running = true;
    if (setjmp(thread_end) == 0) { als_thread(NULL, NULL, NULL); }
    thread_running = false;
}
static void stable_reading(int32_t value) {
    for (unsigned int i = 0; i < 4; i++) { add_reading(0, 0, value); }
}

int main(void) {
    CHECK(map_light_to_pwm(-1) == 1 && map_light_to_pwm(0) == 1);
    CHECK(map_light_to_pwm(50) == 50 && map_light_to_pwm(100) == 100);
    CHECK(map_light_to_pwm(200) == 100);

    reset_fixture(); avago_apds9960_device.ready = false;
    run_thread(1); CHECK(fetch_calls == 0 && pwm_calls == 0 && actual_pwm == 100);
    reset_fixture(); pwm_leds_device.ready = false;
    run_thread(1); CHECK(fetch_calls == 0 && pwm_calls == 0);

    for (unsigned int in_burst = 0; in_burst < 2; in_burst++) {
        for (unsigned int channel_failure = 0; channel_failure < 2; channel_failure++) {
            reset_fixture();
            if (in_burst) { add_reading(0, 0, 20); }
            add_reading(channel_failure ? 0 : -EIO, channel_failure ? -EIO : 0, 0);
            run_thread(1);
            CHECK(fetch_calls == 1 + in_burst && channel_calls == in_burst + channel_failure);
            CHECK(error_sleeps == 1 && pwm_calls == 0);
            CHECK(current_brightness == 100 && actual_pwm == 100);
        }
    }

    reset_fixture(); add_reading(-EIO, 0, 0); add_reading(-EIO, 0, 0);
    run_thread(2); CHECK(error_sleeps == 2 && fetch_calls == 2 && channel_calls == 0);
    CHECK(pwm_calls == 0 && current_brightness == 100);

    reset_fixture(); add_reading(-EIO, 0, 0); stable_reading(20);
    run_thread(2);
    CHECK(error_sleeps == 1 && current_brightness == 20 && actual_pwm == 20);
    CHECK(pwm_calls == 80 && applied[0] == 99 && applied[79] == 20);
    CHECK(burst_sleeps == 3 && fade_sleeps == 80);

    reset_fixture(); CHECK(bl_fade(100, 97) == 0);
    CHECK(pwm_calls == 3 && applied[0] == 99 && applied[2] == 97 && actual_pwm == 97);
    CHECK(bl_fade(97, 100) == 0 && actual_pwm == 100 && current_brightness == 100);
    CHECK(pwm_calls == 6 && applied[3] == 98 && applied[5] == 100);
    CHECK(bl_fade(100, 100) == 0 && pwm_calls == 6);

    reset_fixture(); fail_pwm_call = 2;
    CHECK(bl_fade(100, 95) == -EIO && current_brightness == 99 && actual_pwm == 99);
    CHECK(pwm_calls == 2 && applied_count == 1);
    fail_pwm_call = 0;
    CHECK(bl_fade(99, 95) == 0 && current_brightness == 95 && actual_pwm == 95);

    reset_fixture(); stable_reading(20); fail_pwm_call = 3;
    run_thread(1);
    CHECK(error_sleeps == 1 && pwm_calls == 3 && current_brightness == 98 && actual_pwm == 98);
    puts("Ambient brightness scenarios passed: unavailable sensor, errors/backoff, recovery, fade endpoint and PWM failures");
    return 0;
}
#else
int main(void) {
    reset_fixture(); CHECK(init_fixed_brightness() == 0 && actual_pwm == 75 && pwm_calls == 1);
    reset_fixture(); pwm_leds_device.ready = false;
    CHECK(init_fixed_brightness() == -ENODEV && pwm_calls == 0 && actual_pwm == 100);
    reset_fixture(); fail_pwm_call = 1;
    CHECK(init_fixed_brightness() == -EIO && actual_pwm == 100 && applied_count == 0);
    puts("Fixed brightness scenarios passed: value preserved and device/PWM errors propagated");
    return 0;
}
#endif
