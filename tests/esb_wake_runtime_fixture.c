/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define IS_ENABLED(option) (option)
#define ARG_UNUSED(value) ((void)(value))
#define CONFIG_TOTEM_ESB_DIAGNOSTICS 1
#define CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START 0
#define printk(...) ((void)printf(__VA_ARGS__))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "wake assertion failed at line %d: %s\n", __LINE__, #condition); \
    exit(EXIT_FAILURE); \
} } while (0)

#define NRFX_RESET_REASON_OFF_MASK (1U << 16)
#define PIN_RESET (1U << 0)
#define WATCHDOG_RESET (1U << 1)
#define SOFTWARE_RESET (1U << 2)
#define USB_WAKE (1U << 20)
static uint32_t pending_reasons;
static unsigned int reads, sleeps, starts;
static uint32_t slept_ms;

#ifdef CONFIG_TOTEM_ESB_GPIO_WAKE_START_DELAY_MS
static uint32_t nrfx_reset_reason_get(void) {
    reads++;
    return pending_reasons;
}
static void nrfx_reset_reason_clear(uint32_t mask) {
    pending_reasons &= ~mask;
}
#endif

#if CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS > 0
static void k_msleep(uint32_t ms) {
    CHECK(starts == 0);
    sleeps++;
    slept_ms = ms;
}
#endif

static const unsigned int peripheral_id = 1;
static int zmk_split_esb_peripheral_init(void) {
    starts++;
    return 0;
}

/* ACTUAL_STARTUP_PATH */

static void boot(uint32_t new_reason, uint32_t expected_ms) {
    /* Nordic accumulates flags across resets until the application clears them. */
    pending_reasons |= new_reason;
    reads = sleeps = starts = 0;
    slept_ms = 0;
    peripheral_start_thread_cb(NULL, NULL, NULL);
    CHECK(starts == 1 && slept_ms == expected_ms);
    CHECK(sleeps <= 1 && (expected_ms == 0 || sleeps == 1));
#ifdef CONFIG_TOTEM_ESB_GPIO_WAKE_START_DELAY_MS
    CHECK(reads == 1 && pending_reasons == 0);
#else
    CHECK(reads == 0);
#endif
}

int main(void) {
    const uint32_t cold_ms = CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS;
    boot(0, cold_ms);
    boot(NRFX_RESET_REASON_OFF_MASK, EXPECTED_GPIO_WAKE_MS);
    boot(SOFTWARE_RESET, cold_ms);
    boot(PIN_RESET, cold_ms);
    boot(WATCHDOG_RESET, cold_ms);
    boot(USB_WAKE, cold_ms);
    boot(NRFX_RESET_REASON_OFF_MASK | PIN_RESET, cold_ms);
    boot(NRFX_RESET_REASON_OFF_MASK | (1U << 31), cold_ms);
    boot(NRFX_RESET_REASON_OFF_MASK, EXPECTED_GPIO_WAKE_MS);
    return 0;
}
