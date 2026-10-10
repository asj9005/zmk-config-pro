/* SPDX-License-Identifier: MIT
 * Actual field core with a controlled USB FIFO, clock and scheduling boundary.
 * No MCU timing, USB host scheduling, entropy quality or radio is emulated.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <totem/field_stats.h>
#define CONFIG_TOTEM_FIELD_DIAGNOSTICS 1
#include <totem/field_diagnostics.h>
#include <totem/esb_diagnostics.h>
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_SYS_CLOCK_TICKS_PER_SEC 32768
#define CONFIG_TOTEM_FIELD_INTERVAL_MS 30000
#define CONFIG_TOTEM_FIELD_THREAD_PRIORITY 12
#define TOTEM_FIELD_BUILD_SHA "1111111111111111111111111111111111111111"
#define TOTEM_FIELD_BUILD_TREE "2222222222222222222222222222222222222222"
#define TOTEM_FIELD_BUILD_DIRTY 0
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BUILD_ASSERT(x, ...) _Static_assert((x), #x)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define K_MSEC(ms) (ms)
#define APPLICATION 0
#define DT_HAS_CHOSEN(x) 1
#define DT_NODE_HAS_COMPAT(a,b) 1
#define DT_CHOSEN(x) 1
#define SYS_INIT(...)
#define K_THREAD_STACK_DEFINE(name, size) unsigned char name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #x); exit(87); } } while(0)
static uint64_t now_ms;
static unsigned int locks, lock_depth, fifo_calls, scheduled_calls;
static bool connected = true, ready = true, suspended;
static int fifo_limit = 64;
static char captured[200000];
static size_t captured_length;
static int64_t scheduled_delay;
struct device { int unused; };
static struct device fake_device;
#define DEVICE_DT_GET(x) (&fake_device)
static bool device_is_ready(const struct device *dev) { (void)dev; return ready; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) { (void)lock; CHECK(lock_depth++ == 0); locks++; return 0; }
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) { (void)lock; (void)key; CHECK(lock_depth-- == 1); }
static int64_t k_uptime_get(void) { return now_ms; }
static int64_t k_uptime_ticks(void) { return now_ms * 32768 / 1000; }
static uint64_t k_ticks_to_us_floor64(int64_t ticks) { return (uint64_t)ticks * 1000000 / 32768; }
static uint32_t sys_rand32_get(void) { static uint32_t id = 0xabcdef00; return id++; }
enum { USB_DC_CONFIGURED, USB_DC_SUSPEND };
static int zmk_usb_get_status(void) { return suspended ? USB_DC_SUSPEND : USB_DC_CONFIGURED; }
static bool zmk_usb_is_hid_ready(void) { return connected; }
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; };
struct k_work_q { int unused; };
#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name = {{fn}}
static void k_work_queue_start(struct k_work_q *q, unsigned char *stack, size_t size, int priority, void *config) {
    (void)q; (void)stack; (void)config; CHECK(size == 2048 && priority == 12);
}
static int k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *work, int64_t delay) {
    (void)q; (void)work; CHECK(lock_depth == 0 && delay > 0); scheduled_delay = delay; scheduled_calls++; return 1;
}
static int vsnprintk(char *buffer, size_t size, const char *format, va_list args) { return vsnprintf(buffer, size, format, args); }
static int uart_fifo_fill(const struct device *dev, const uint8_t *data, int length) {
    CHECK(dev == &fake_device && lock_depth == 0 && length > 0 && length <= 64);
    fifo_calls++;
    if (fifo_limit < 0) return fifo_limit;
    unsigned int accepted = (unsigned int)MIN(length, fifo_limit);
    CHECK(captured_length + accepted < sizeof(captured));
    memcpy(captured + captured_length, data, accepted); captured_length += accepted;
    captured[captured_length] = 0;
    return (int)accepted;
}
struct esb_diagnostics { uint32_t state, radio_state, tx_queued, retries, irq_flags, radio_events,
    timer_events, timer_shorts, radio_irq, timer_irq, late_ack_setup; };
static int esb_get_diagnostics(struct esb_diagnostics *r) { memset(r, 0, sizeof(*r)); r->state = 4; return 0; }
void totem_esb_diag_snapshot(struct totem_esb_diag_snapshot *d) {
    memset(d, 0, sizeof(*d)); d->stages[TOTEM_DIAG_BOOT_NONCE] = INT32_MIN;
    d->events[TOTEM_DIAG_RX] = 99; d->rx_age_max = 8;
}

/* ACTUAL_FIELD_CORE */

static void reset(void) {
    memset(&live, 0, sizeof(live)); memset(&snapshot, 0, sizeof(snapshot));
    memset(&output, 0, sizeof(output));
    now_ms = 1000; locks = lock_depth = fifo_calls = scheduled_calls = 0;
    connected = ready = true; suspended = false; fifo_limit = 64;
    captured_length = 0; captured[0] = 0; next_snapshot_ms = 0;
    part = line_length = line_offset = 0; boot_initialized = false;
    CHECK(field_init() == 0);
}
static void run_frame(void) {
    unsigned int turns = 0;
    do {
        field_work_handler(&field_work.work);
        now_ms++;
        CHECK(++turns <= FIELD_TX_BUDGET_MS + 1);
    } while(output.active);
}
static unsigned int count_lines(const char *marker) {
    unsigned int n = 0; const char *p = captured;
    while ((p = strstr(p, marker))) { n++; p++; }
    return n;
}
static void stats_test(void) {
    struct totem_field_stats stats;
    totem_field_stats_reset(&stats);
    for (unsigned int i = 0; i < TOTEM_FIELD_BINS - 1; i++) {
        CHECK(totem_field_stats_observe(&stats, totem_field_bin_upper_us[i]));
        CHECK(stats.bins[i] == 1);
    }
    CHECK(totem_field_stats_observe(&stats, 1000001));
    CHECK(stats.count == 18 && stats.bins[17] == 1);
    totem_field_stats_reset(&stats);
    CHECK(stats.count == 0 && stats.sum_us == 0 && stats.max_us == 0 && stats.flags == 0);
    CHECK(totem_field_stats_observe(&stats, 0));
    CHECK(stats.bins[0] == 1);
    stats.sum_us = UINT64_MAX - 10;
    CHECK(totem_field_stats_observe(&stats, UINT64_MAX));
    CHECK(stats.sum_us == UINT64_MAX && stats.max_us == UINT32_MAX && stats.flags == 6);
    stats.count = UINT32_MAX;
    uint32_t old_bin = stats.bins[0];
    CHECK(!totem_field_stats_observe(&stats, 1));
    CHECK(stats.count == UINT32_MAX && stats.bins[0] == old_bin && stats.flags == 7);
    CHECK(totem_field_sat_inc(UINT32_MAX) == UINT32_MAX);
}
int main(int argc, char **argv) {
    (void)argv;
    stats_test();
    reset();
    totem_field_observe(TOTEM_FIELD_RX_QUEUE, 1000);
    totem_field_observe(TOTEM_FIELD_HOLD_TAP_BASE_E, 180000);
    totem_field_issue(TOTEM_FIELD_PEER_TIMEOUT, TOTEM_FIELD_SCOPE_LEFT, -2);
    totem_field_hold_tap(TOTEM_FIELD_BASE_E, false);
    totem_field_hold_tap(TOTEM_FIELD_MOUSE_FAST, true);
    totem_field_observe(TOTEM_FIELD_METRIC_COUNT, 999);
    totem_field_issue(TOTEM_FIELD_REASON_COUNT, 0, 999);
    totem_field_issue(TOTEM_FIELD_PEER_TIMEOUT, TOTEM_FIELD_SCOPE_COUNT, 999);
    totem_field_hold_tap(TOTEM_FIELD_GROUP_COUNT, true);
    CHECK(live.metrics[TOTEM_FIELD_RX_QUEUE].count == 1);
    CHECK(live.issues[TOTEM_FIELD_PEER_TIMEOUT][0].count == 1);
    CHECK(live.holds[TOTEM_FIELD_BASE_E][0] == 1 && live.holds[TOTEM_FIELD_MOUSE_FAST][1] == 1);
    run_frame();
    CHECK(output.dropped == 0 && output.sequence == 1 && count_lines("[totem-field]") == 72);
    CHECK(strstr(captured, "format=TOTEM_FIELD_V1 fw=" TOTEM_FIELD_BUILD_SHA " tree=" TOTEM_FIELD_BUILD_TREE " dirty=0"));
    CHECK(strstr(captured, "part=71 total=72 kind=end"));
    CHECK(strstr(captured, "name=rx_queue count=1 sum_us=1000 max_us=1000 flags=0 b0=0 b1=0 b2=0 b3=1"));
    CHECK(strstr(captured, "name=base_e tap=1 hold=0"));
    CHECK(strstr(captured, "name=mouse_fast tap=0 hold=1"));
    CHECK(strstr(captured, "name=peer_timeout scope=0 count=1 last_ms=1000 code=-2"));
    CHECK(scheduled_delay > 28000 && locks > 0);
    if (argc > 1) { fputs(captured, stdout); return 0; }

    reset(); fifo_limit = 24; run_frame();
    CHECK(output.dropped == 0 && count_lines("[totem-field]") == 72);
    CHECK(strstr(captured, "part=71 total=72 kind=end"));

    reset(); fifo_limit = 0; run_frame();
    CHECK(output.dropped == 1 && captured_length == 0 && fifo_calls == FIELD_TX_BUDGET_MS);
    CHECK(scheduled_delay >= 29000);
    now_ms = next_snapshot_ms; fifo_limit = 64; run_frame();
    CHECK(output.sequence == 2 && output.dropped == 1 && strstr(captured, "tx_drop=1"));

    reset(); field_work_handler(&field_work.work); CHECK(output.active && captured_length == 64);
    connected = false; now_ms++; field_work_handler(&field_work.work);
    CHECK(output.dropped == 1 && !output.active && fifo_calls == 1);
    connected = true; fifo_limit = 64; now_ms = next_snapshot_ms; run_frame();
    CHECK(strstr(captured + 64, "\n[totem-field] schema=1") == captured + 64);
    CHECK(output.sequence == 2 && output.dropped == 1);

    reset(); suspended = true; run_frame(); CHECK(fifo_calls == 0 && output.dropped == 1);
    reset(); connected = false; run_frame(); CHECK(fifo_calls == 0 && output.dropped == 1);
    reset(); fifo_limit = -5; run_frame(); CHECK(fifo_calls == 1 && output.dropped == 1);
    reset(); ready = false; scheduled_calls = 0; CHECK(field_init() == 0 && scheduled_calls == 0);

    reset(); live.holds[0][0] = UINT32_MAX; totem_field_hold_tap(TOTEM_FIELD_BASE_E, false);
    CHECK(live.holds[0][0] == UINT32_MAX);
    live.issues[0][0].count = UINT32_MAX; now_ms = 1234;
    totem_field_issue(TOTEM_FIELD_PEER_TIMEOUT, 0, -3);
    CHECK(live.issues[0][0].count == UINT32_MAX && live.issues[0][0].last_ms == 1234);
    output.sequence = UINT32_MAX; run_frame(); CHECK(output.sequence == 1);
    CHECK(totem_field_now_us() <= now_ms * 1000 && now_ms * 1000 - totem_field_now_us() < 31);
    puts("actual field diagnostics stats, metadata and bounded CDC scenarios passed");
    return 0;
}
