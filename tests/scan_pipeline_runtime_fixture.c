/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ZMK_KEYMAP_LEN 38
#define CONFIG_ZMK_KSCAN_EVENT_QUEUE_SIZE 32
#define ZMK_KSCAN_EVENT_STATE_PRESSED 0
#define ZMK_KSCAN_EVENT_STATE_RELEASED 1
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL 255
#define K_NO_WAIT 0
#define ARG_UNUSED(x) (void)(x)
enum { TOTEM_DIAG_SCAN_OVERFLOW, TOTEM_DIAG_SCAN_RESYNC };
struct k_work { int unused; };
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
/* ACTUAL_SCAN_EVENT */
static struct { struct k_work work; } msg_processor;
static struct { unsigned int head, count; struct zmk_kscan_event events[32]; } storage;
static int physical_layouts_kscan_msgq;
static struct k_spinlock scan_state_lock;
static bool scan_physical[ZMK_KEYMAP_LEN], scan_applied[ZMK_KEYMAP_LEN], scan_resync_pending;
static struct { int matrix_transform; } layout;
static const void *active_dummy;
#define active (&layout)
static int64_t now_ms, last_timestamp;
static unsigned int diag[2], scheduled, delivered, high_water;
static bool consumer_state[ZMK_KEYMAP_LEN];
static bool inject_on_snapshot_release;
static void key_event(uint32_t position, bool pressed);
struct zmk_position_state_changed { unsigned int source; bool state; uint32_t position; int64_t timestamp; };
static int k_spin_lock(struct k_spinlock *lock) { (void)lock; return 0; }
static void k_spin_unlock(struct k_spinlock *lock, int key) { (void)lock; (void)key; }
static int64_t k_uptime_get(void) { return now_ms; }
static int32_t zmk_matrix_transform_row_column_to_position(int transform, uint32_t row, uint32_t column) {
    (void)transform; (void)row; return column < ZMK_KEYMAP_LEN ? (int32_t)column : -1;
}
static int k_msgq_put(int *queue, const struct zmk_kscan_event *event, int timeout) {
    (void)queue; assert(timeout == K_NO_WAIT);
    if (storage.count == 32) { return -ENOMSG; }
    storage.events[(storage.head + storage.count++) % 32] = *event; return 0;
}
static int k_msgq_get(int *queue, struct zmk_kscan_event *event, int timeout) {
    (void)queue; assert(timeout == K_NO_WAIT);
    if (!storage.count) { return -ENOMSG; }
    *event = storage.events[storage.head]; storage.head = (storage.head + 1) % 32; storage.count--; return 0;
}
static uint32_t k_msgq_num_used_get(int *queue) { (void)queue; return storage.count; }
static int k_work_submit(struct k_work *work) { (void)work; scheduled++; return 0; }
static void totem_esb_diag_event(int event, int value) { (void)value; diag[event]++; }
static void totem_esb_diag_scan_observe(uint32_t used) { if (used > high_water) { high_water = used; } }
static void raise_zmk_position_state_changed(struct zmk_position_state_changed event) {
    assert(event.position < ZMK_KEYMAP_LEN);
    assert(consumer_state[event.position] != event.state);
    consumer_state[event.position] = event.state; last_timestamp = event.timestamp; delivered++;
    if (inject_on_snapshot_release && event.position == 0 && !event.state) {
        inject_on_snapshot_release = false;
        key_event(0, true);
        key_event(2, true);
    }
}
/* ACTUAL_CALLBACKS */
static void reset(void) {
    memset(&storage, 0, sizeof(storage)); memset(scan_physical, 0, sizeof(scan_physical));
    memset(scan_applied, 0, sizeof(scan_applied)); memset(consumer_state, 0, sizeof(consumer_state));
    memset(diag, 0, sizeof(diag)); scan_resync_pending = false;
    now_ms = 100; last_timestamp = 0; delivered = scheduled = high_water = 0;
    inject_on_snapshot_release = false;
    (void)active_dummy;
}
static void key_event(uint32_t position, bool pressed) {
    queue_scan_event((struct zmk_kscan_event){.row=0, .column=position,
                     .state=pressed ? ZMK_KSCAN_EVENT_STATE_PRESSED : ZMK_KSCAN_EVENT_STATE_RELEASED});
}
static void run(void) { zmk_physical_layouts_kscan_process_msgq(&msg_processor.work); }
int main(void) {
    reset();
    for (uint32_t i=0; i<19; i++) { key_event(i, true); }
    run(); assert(delivered == 19 && diag[0] == 0);
    for (uint32_t i=0; i<19; i++) { key_event(i, false); }
    run(); assert(delivered == 38 && diag[0] == 0);

    reset();
    for (uint32_t i=0; i<38; i++) { key_event(i, true); }
    run(); assert(delivered == 38 && diag[0] == 6 && diag[1] == 1);
    for (uint32_t i=0; i<38; i++) { key_event(i, false); }
    run(); assert(delivered == 76 && diag[0] == 12 && diag[1] == 2);
    for (uint32_t i=0; i<38; i++) { assert(!consumer_state[i]); }

    reset(); key_event(0, true); run();
    for (uint32_t i=0; i<32; i++) { key_event(1, (i % 2) == 0); }
    key_event(0, false); run();
    assert(!consumer_state[0] && !consumer_state[1] && diag[1] == 1);

    reset(); key_event(0, true); now_ms = 900; run(); assert(last_timestamp == 100);
    key_event(38, true); key_event(255, false); run(); assert(delivered == 1);
    key_event(0, true); run(); assert(delivered == 1);
    key_event(0, false); run(); assert(delivered == 2 && !consumer_state[0]);
    /* Admission during snapshot dispatch must follow, never precede, the
     * captured recovery state. The work submission is retained while running. */
    reset(); key_event(0, true); run();
    for (uint32_t i=0; i<32; i++) { key_event(1, (i % 2) == 0); }
    key_event(0, false);
    inject_on_snapshot_release = true;
    run();
    assert(!consumer_state[0] && !consumer_state[2] && storage.count == 2);
    run(); assert(consumer_state[0] && consumer_state[2] && storage.count == 0);
    key_event(0, false); key_event(2, false); run();
    assert(!consumer_state[0] && !consumer_state[2]);
    puts("5 actual scan pipeline scenarios passed");
    return 0;
}
