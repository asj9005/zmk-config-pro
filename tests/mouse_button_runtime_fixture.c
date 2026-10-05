/* SPDX-License-Identifier: MIT */
/* Actual button callbacks with bounded input admission and sync accumulation.
 * The boundary fake models input queue capacity, not Zephyr thread timing. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define ZMK_HID_MOUSE_NUM_BUTTONS 5
#define INPUT_BTN_0 256
#define BIT(n) (1U << (n))
#define BUILD_ASSERT(x) _Static_assert(x, #x)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define CONTAINER_OF(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define LOG_DBG(...) ((void)0)
static const char *case_name;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", case_name, __LINE__, #x); exit(1); } } while (0)
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; void (*callback)(struct k_work *); bool pending; int delay; };
struct k_spinlock { bool held; };
typedef int k_spinlock_key_t;
struct device { void *data; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1; };
struct zmk_behavior_binding_event { uint32_t position; };
enum { TOTEM_DIAG_INPUT_RETRY, TOTEM_DIAG_INPUT_OVERFLOW };
static const struct device *fake_device;
static unsigned int diag[2], calls, reports;
static int input_slots;
static uint8_t set_bits, clear_bits, hid_buttons;
static uint32_t hid_counts[5];
static uint8_t report_states[256];
static bool release_during_write;
static void process_key_state(const struct device *dev, int32_t val, bool pressed);
static int k_spin_lock(struct k_spinlock *lock) { CHECK(!lock->held); lock->held = true; return 0; }
static void k_spin_unlock(struct k_spinlock *lock, int key) { (void)key; CHECK(lock->held); lock->held = false; }
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static void k_work_init_delayable(struct k_work_delayable *work, void (*callback)(struct k_work *)) {
    work->callback = callback;
}
static int k_work_reschedule(struct k_work_delayable *work, int delay) {
    CHECK(delay >= 0); work->pending = true; work->delay = delay; return 1;
}
static void totem_esb_diag_event(int event, int result) { (void)result; CHECK(event < 2); diag[event]++; }
static const struct device *zmk_behavior_get_binding(const char *name) { CHECK(strcmp(name, "mkp") == 0); return fake_device; }
static int input_report_key(const struct device *dev, uint16_t code, int32_t value, bool sync, int timeout) {
    CHECK(dev == fake_device && timeout == K_NO_WAIT);
    CHECK(code >= INPUT_BTN_0 && code < INPUT_BTN_0 + 5);
    calls++;
    if (input_slots == 0) { return -ENOMSG; }
    if (input_slots > 0) { input_slots--; }
    uint8_t bit = BIT(code - INPUT_BTN_0);
    if (value) { set_bits |= bit; } else { clear_bits |= bit; }
    if (sync) {
        for (unsigned int i = 0; i < 5; i++) {
            if (set_bits & BIT(i)) { hid_counts[i]++; }
            if (clear_bits & BIT(i)) { CHECK(hid_counts[i] != 0); hid_counts[i]--; }
            if (hid_counts[i]) { hid_buttons |= BIT(i); } else { hid_buttons &= (uint8_t)~BIT(i); }
        }
        set_bits = clear_bits = 0;
        CHECK(reports < sizeof(report_states)); report_states[reports++] = hid_buttons;
    }
    if (release_during_write) {
        release_during_write = false;
        process_key_state(dev, 1, false);
    }
    return 0;
}
/* ACTUAL_BUTTON_BEHAVIOR */
static struct mouse_button_data data;
static struct device dev;
static void reset_case(const char *name) {
    case_name = name; memset(&data, 0, sizeof(data)); memset(diag, 0, sizeof(diag));
    dev.data = &data; fake_device = &dev;
    calls = reports = 0; input_slots = -1;
    set_bits = clear_bits = hid_buttons = 0; release_during_write = false;
    memset(hid_counts, 0, sizeof(hid_counts));
    CHECK(mouse_button_init(&dev) == 0);
}
static void key(uint32_t mask, bool pressed) {
    struct zmk_behavior_binding binding = {"mkp", mask};
    struct zmk_behavior_binding_event event = {1};
    CHECK((pressed ? on_keymap_binding_pressed(&binding, event) : on_keymap_binding_released(&binding, event)) == 0);
}
static void run(void) {
    CHECK(data.work.pending); data.work.pending = false; unsigned int before = calls;
    data.work.callback(&data.work.work); CHECK(calls - before <= MOUSE_BUTTON_BATCH_SIZE);
    CHECK(data.count <= MOUSE_BUTTON_QUEUE_SIZE && !data.lock.held);
}
static void drain(void) {
    for (int i = 0; data.work.pending && i < 100; i++) { run(); }
    CHECK(!data.work.pending && data.count == 0 && !data.resync);
    CHECK(!set_bits && !clear_bits);
    for (int i = 0; i < 5; i++) { CHECK(data.accepted[i] == data.desired[i]); }
}
static void final_release_retried_without_new_input(void) {
    reset_case("final_release_retried_without_new_input");
    key(1, true); drain(); CHECK(hid_buttons == 1);
    input_slots = 0; key(1, false); run(); run();
    CHECK(data.count == 1 && data.work.delay == 4 && hid_buttons == 1 && diag[0] == 2);
    input_slots = 1; drain(); CHECK(hid_buttons == 0 && reports == 2);
    CHECK(report_states[0] == 1 && report_states[1] == 0);
}
static void tap_fifo_under_backpressure(void) {
    reset_case("tap_fifo_under_backpressure");
    input_slots = 0; key(1, true); key(1, false); run();
    CHECK(data.count == 2 && reports == 0);
    input_slots = 1; run(); CHECK(hid_buttons == 1 && data.count == 1);
    input_slots = 1; drain(); CHECK(hid_buttons == 0 && reports == 2);
}
static void combined_masks_keep_final_sync(void) {
    reset_case("combined_masks_keep_final_sync");
    key(7, true); input_slots = 1; run();
    CHECK(reports == 0 && set_bits == 1 && data.accepted[0] == 1 && data.count == 1);
    key(7, false); input_slots = 2; run();
    CHECK(reports == 1 && report_states[0] == 7 && !set_bits && data.count == 1);
    input_slots = 1; run(); CHECK(reports == 1 && clear_bits == 1);
    input_slots = 2; drain(); CHECK(reports == 2 && hid_buttons == 0);
}
static void overflow_keeps_last_release(void) {
    reset_case("overflow_keeps_last_release");
    key(1, true); drain(); CHECK(hid_buttons == 1);
    input_slots = 0;
    for (int i = 0; i < 64; i++) { key(2, (i % 2) == 0); }
    key(1, false); key(4, true); key(4, false);
    CHECK(data.count == 64 && data.resync && diag[1] == 3 && data.desired[0] == 0);
    run(); CHECK(data.count == 64 && data.work.delay == 4);
    input_slots = -1; drain();
    CHECK(hid_buttons == 0 && reports == 66 && report_states[65] == 0);
}
static void overflow_recovers_mixed_button_state(void) {
    reset_case("overflow_recovers_mixed_button_state");
    key(1, true); drain();
    for (int i = 0; i < 64; i++) { key(2, (i % 2) == 0); }
    key(1, false); key(16, true);
    drain(); CHECK(hid_buttons == 16 && reports == 66 && report_states[65] == 16);
    key(16, false); drain(); CHECK(hid_buttons == 0);
}
static void producer_cannot_replace_inflight_head(void) {
    reset_case("producer_cannot_replace_inflight_head");
    key(1, true); release_during_write = true; drain();
    CHECK(reports == 2 && report_states[0] == 1 && report_states[1] == 0 && hid_buttons == 0);
}
static void invalid_bits_and_bounded_batches(void) {
    reset_case("invalid_bits_and_bounded_batches");
    key(0, true); key(32, true); CHECK(!data.work.pending && !data.count);
    key(32 | 16, true); key(16, false); drain(); CHECK(reports == 2 && hid_buttons == 0);
    for (int i = 0; i < 12; i++) { key(1, (i % 2) == 0); }
    unsigned int before = calls; run();
    CHECK(calls - before == 8 && data.count == 4 && data.work.pending && data.work.delay == 0);
    drain(); CHECK(hid_buttons == 0);
}
static void overlapping_button_owners_survive_overflow(void) {
    reset_case("overlapping_button_owners_survive_overflow");
    /* Model an external pointer's reference, owned outside &mkp. */
    hid_counts[1] = 1; hid_buttons = 2;
    key(2, true); key(2, true); drain(); CHECK(hid_counts[1] == 3);
    key(2, false); drain(); CHECK(hid_counts[1] == 2 && hid_buttons == 2);
    key(2, true); drain(); CHECK(hid_counts[1] == 3);
    for (int i = 0; i < 64; i++) { key(1, (i % 2) == 0); }
    key(2, false); key(2, false);
    CHECK(data.resync && data.desired[1] == 0 && data.accepted[1] == 2);
    drain(); CHECK(hid_counts[1] == 1 && hid_buttons == 2);
    key(2, false); CHECK(!data.work.pending && hid_counts[1] == 1);
}
int main(void) {
    final_release_retried_without_new_input(); tap_fifo_under_backpressure();
    combined_masks_keep_final_sync(); overflow_keeps_last_release();
    overflow_recovers_mixed_button_state(); producer_cannot_replace_inflight_head();
    invalid_bits_and_bounded_batches();
    overlapping_button_owners_survive_overflow();
    puts("8 actual mouse button cases passed"); return 0;
}
