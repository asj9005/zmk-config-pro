/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <totem/hid_report_queue.h>
#define ARG_UNUSED(x) (void)(x)
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(x) (x)
#define CONFIG_ZMK_USB_BOOT 1
#define CONFIG_ZMK_USB 1
#define CONFIG_ZMK_POINTING 1
#define IS_ENABLED(x) (x)
#define LOG_DBG(...) ((void)0)
#define HID_PROTOCOL_REPORT 1
#define HID_PROTOCOL_BOOT 0
struct k_work { int unused; };
struct k_spinlock { int held; };
struct device { int unused; };
typedef int k_spinlock_key_t;
static struct totem_hid_queue report_queue;
static struct k_spinlock report_queue_lock;
static struct k_work usb_report_work, usb_status_notifier_work;
static struct device device_instance;
static const struct device *hid_dev = &device_instance;
static int hid_sem = 1, usb_endpoint_lock;
static struct totem_hid_packet in_flight_packet;
static bool in_flight, reports_need_resync;
static uint8_t hid_protocol = HID_PROTOCOL_REPORT;
enum usb_dc_status_code { USB_DC_SUSPEND, USB_DC_ERROR, USB_DC_RESET, USB_DC_DISCONNECTED,
    USB_DC_UNKNOWN, USB_DC_CONFIGURED, USB_DC_RESUME, USB_DC_CLEAR_HALT, USB_DC_SOF,
    USB_DC_CONNECTED };
enum zmk_usb_conn_state { ZMK_USB_CONN_NONE, ZMK_USB_CONN_POWERED, ZMK_USB_CONN_HID };
static enum usb_dc_status_code usb_status = USB_DC_CONFIGURED;
static bool is_configured = true;
enum { TOTEM_DIAG_USB_RETRY, TOTEM_DIAG_USB_OVERFLOW };
static unsigned int diagnostics[2], high_water, scheduled, delay_ms, writes, wakes;
static int write_error, deferred_protocol = -1;
static bool replace_generation_on_write;
static struct totem_hid_packet sent[256];
static const uint8_t *dma_pointer;
static size_t dma_length;
static unsigned int dma_slot;
static void set_proto_cb(const struct device *dev, uint8_t protocol);
static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *params);
static enum usb_dc_status_code zmk_usb_get_status(void);
static bool zmk_usb_is_hid_ready(void);
static int k_spin_lock(struct k_spinlock *lock) { assert(!lock->held); lock->held = 1; return 0; }
static void k_spin_unlock(struct k_spinlock *lock, int key) {
    (void)key; assert(lock->held); lock->held = 0;
}
static int k_mutex_lock(int *mutex, int delay) {
    if (*mutex) { assert(delay == K_NO_WAIT); return -EBUSY; }
    *mutex = 1; return 0;
}
static void k_mutex_unlock(int *mutex) {
    assert(*mutex); *mutex = 0;
    if (deferred_protocol >= 0) {
        int protocol = deferred_protocol; deferred_protocol = -1;
        set_proto_cb(hid_dev, (uint8_t)protocol);
    }
}
static int k_work_reschedule(struct k_work *work, int delay) {
    (void)work; scheduled++; delay_ms = (unsigned int)delay; return 0;
}
static int k_work_submit(struct k_work *work) { (void)work; return 0; }
static int k_sem_take(int *sem, int delay) {
    assert(delay == K_NO_WAIT);
    if (!*sem) { return -EAGAIN; }
    *sem = 0; return 0;
}
static void k_sem_give(int *sem) { *sem = 1; }
static int usb_wakeup_request(void) { wakes++; return 0; }
static int hid_int_ep_write(const struct device *dev, const uint8_t *data, size_t len, void *unused) {
    (void)dev; (void)unused;
    assert(usb_endpoint_lock && !report_queue_lock.held);
    if (write_error) { return write_error; }
    assert(!dma_pointer && writes < 256);
    /* Retain the pointer: nrfx does not copy this buffer on submission. */
    assert(data == in_flight_packet.data);
    dma_pointer = data; dma_length = len; dma_slot = writes++;
    if (replace_generation_on_write) {
        uint8_t replacement[] = {1, 77};
        totem_hid_queue_reset(&report_queue);
        assert(totem_hid_queue_offer(&report_queue, TOTEM_HID_KEYBOARD,
                                    replacement, replacement, 2) == 0);
        replace_generation_on_write = false;
    }
    return 0;
}
static void totem_esb_diag_event(int event, int value) { (void)value; diagnostics[event]++; }
static void totem_esb_diag_usb_observe(uint32_t used) { if (used > high_water) { high_water = used; } }
struct zmk_hid_keyboard_report { uint8_t report_id, key; };
struct zmk_hid_consumer_report { uint8_t report_id, key; };
typedef struct { uint8_t modifier, reserved, keys[6]; } zmk_hid_boot_report_t;
struct zmk_hid_mouse_report {
    uint8_t report_id;
    struct { uint8_t buttons; int16_t d_x, d_y, d_scroll_y, d_scroll_x; } body;
};
static struct zmk_hid_keyboard_report keyboard_report;
static struct zmk_hid_consumer_report consumer_report;
static zmk_hid_boot_report_t boot_report;
static struct zmk_hid_mouse_report mouse_report;
static struct zmk_hid_keyboard_report *zmk_hid_get_keyboard_report(void) { return &keyboard_report; }
static struct zmk_hid_consumer_report *zmk_hid_get_consumer_report(void) { return &consumer_report; }
static zmk_hid_boot_report_t *zmk_hid_get_boot_report(void) { return &boot_report; }
static struct zmk_hid_mouse_report *zmk_hid_get_mouse_report(void) { return &mouse_report; }
static void usb_report_work_cb(struct k_work *work);
/* ACTUAL_CALLBACKS */
/* ACTUAL_USB_STATUS */

static void reset(void) {
    memset(&report_queue, 0, sizeof(report_queue));
    memset(&in_flight_packet, 0, sizeof(in_flight_packet));
    memset(diagnostics, 0, sizeof(diagnostics)); memset(sent, 0, sizeof(sent));
    memset(&mouse_report, 0, sizeof(mouse_report)); memset(&boot_report, 0, sizeof(boot_report));
    keyboard_report = (struct zmk_hid_keyboard_report){1, 0};
    consumer_report = (struct zmk_hid_consumer_report){2, 0}; mouse_report.report_id = 3;
    high_water = scheduled = delay_ms = writes = wakes = 0;
    hid_sem = 1; write_error = usb_endpoint_lock = report_queue_lock.held = 0;
    deferred_protocol = -1; replace_generation_on_write = false;
    usb_status = USB_DC_CONFIGURED; is_configured = true;
    in_flight = reports_need_resync = false; hid_protocol = HID_PROTOCOL_REPORT;
    dma_pointer = NULL; dma_length = 0;
}
static void offer(uint8_t value) {
    keyboard_report.key = value; boot_report.keys[0] = value;
    assert(zmk_usb_hid_send_keyboard_report() == 0);
}
static void complete(void) {
    assert(dma_pointer);
    sent[dma_slot].length = (uint8_t)dma_length;
    memcpy(sent[dma_slot].data, dma_pointer, dma_length);
    dma_pointer = NULL; in_ready_cb(hid_dev);
}
static void abort_and_notify(enum usb_dc_status_code status) {
    /* The pinned driver aborts DMA before delivering these status callbacks. */
    dma_pointer = NULL; usb_status_cb(status, NULL);
}
static void drain(void) {
    unsigned int limit = 250;
    while (totem_hid_queue_pending(&report_queue) || in_flight || reports_need_resync) {
        assert(limit--);
        if (dma_pointer) { complete(); }
        usb_report_work_cb(&usb_report_work);
    }
}
int main(void) {
    reset(); offer(4); offer(0);
    assert(writes == 0 && report_queue.count == 2);
    drain(); assert(writes == 2 && sent[0].data[1] == 4 && sent[1].data[1] == 0);

    reset(); offer(7); usb_report_work_cb(&usb_report_work); offer(0);
    usb_report_work_cb(&usb_report_work);
    assert(writes == 1 && report_queue.count == 1 && delay_ms == 1 && hid_sem == 0);
    complete(); write_error = -EIO; usb_report_work_cb(&usb_report_work);
    assert(writes == 1 && report_queue.count == 1 && diagnostics[TOTEM_DIAG_USB_RETRY] == 1);
    write_error = 0; drain(); assert(writes == 2 && sent[1].data[1] == 0);

    reset(); offer(7); usb_status_cb(USB_DC_SUSPEND, NULL); usb_report_work_cb(&usb_report_work);
    assert(wakes == 1 && writes == 0 && report_queue.count == 1 && delay_ms == 10);
    usb_status_cb(USB_DC_RESUME, NULL); drain(); assert(sent[0].data[1] == 7);

    reset();
    for (unsigned int i = 0; i < TOTEM_HID_QUEUE_SLOTS; i++) { offer((uint8_t)(i + 1)); }
    offer(99); offer(0);
    assert(high_water == 64 && diagnostics[TOTEM_DIAG_USB_OVERFLOW] == 2);
    drain(); assert(writes == 65 && sent[64].data[1] == 0);

    reset();
    for (unsigned int i = 0; i < TOTEM_HID_QUEUE_SLOTS; i++) { offer(1); }
    mouse_report.body.d_x = 60; mouse_report.body.d_y = -20;
    mouse_report.body.d_scroll_x = 1; mouse_report.body.d_scroll_y = -1;
    assert(zmk_usb_hid_send_mouse_report() == 0);
    drain(); struct zmk_hid_mouse_report repaired;
    memcpy(&repaired, sent[64].data, sizeof(repaired));
    assert(repaired.body.buttons == 0 && repaired.body.d_x == 0 && repaired.body.d_y == 0);
    assert(repaired.body.d_scroll_x == 0 && repaired.body.d_scroll_y == 0);

    reset(); offer(4); assert(zmk_usb_hid_send_consumer_report() == 0); offer(0);
    drain(); assert(writes == 3 && sent[1].data[0] == 2 && sent[2].data[1] == 0);
    assert(zmk_usb_hid_queue_report(TOTEM_HID_KEYBOARD, sent[0].data, sent[0].data, 65) == -EMSGSIZE);

    /* DMA bytes survive function returns, producer activity, and overflow. */
    reset(); offer(31); usb_report_work_cb(&usb_report_work);
    for (unsigned int i = 0; i < TOTEM_HID_QUEUE_SLOTS + 2; i++) { offer((uint8_t)i); }
    usb_report_work_cb(&usb_report_work); assert(writes == 1 && dma_pointer[1] == 31);
    complete(); assert(sent[0].data[1] == 31); drain();

    const enum usb_dc_status_code aborts[] = {USB_DC_RESET, USB_DC_DISCONNECTED};
    for (unsigned int i = 0; i < 2; i++) {
        reset(); offer(4); usb_report_work_cb(&usb_report_work); offer(0);
        assert(hid_sem == 0); abort_and_notify(aborts[i]);
        assert(hid_sem == 1 && !in_flight && report_queue.count == 0);
        in_ready_cb(hid_dev); usb_report_work_cb(&usb_report_work); assert(writes == 1);
        usb_status_cb(USB_DC_CONNECTED, NULL); usb_report_work_cb(&usb_report_work);
        assert(writes == 1); usb_status_cb(USB_DC_CONFIGURED, NULL); drain();
        assert(writes == 4 && sent[1].data[1] == 0);
    }

    /* Discard old queued formats, but keep the accepted DMA alive until ready. */
    reset(); offer(4); usb_report_work_cb(&usb_report_work); offer(0);
    set_proto_cb(hid_dev, HID_PROTOCOL_BOOT);
    assert(hid_sem == 0 && dma_pointer[0] == 1 && dma_pointer[1] == 4);
    assert(report_queue.count == 0 && reports_need_resync);
    assert(zmk_usb_hid_send_consumer_report() == -ENOTSUP);
    assert(zmk_usb_hid_send_mouse_report() == -ENOTSUP);
    complete(); drain(); assert(writes == 2 && sent[1].length == 8 && sent[1].data[2] == 0);
    offer(12); zmk_usb_hid_set_protocol(HID_PROTOCOL_REPORT); drain();
    assert(writes == 5 && sent[2].length == 2 && sent[2].data[1] == 12);

    /* Contending protocol change becomes effective only after submit/pop. */
    reset(); offer(6); deferred_protocol = HID_PROTOCOL_BOOT; usb_report_work_cb(&usb_report_work);
    assert(writes == 1 && hid_protocol == HID_PROTOCOL_BOOT && report_queue.count == 0);
    assert(dma_pointer[0] == 1 && dma_pointer[1] == 6);
    complete(); drain(); assert(writes == 2 && sent[1].length == 8 && sent[1].data[2] == 6);

    /* A generation replaced during submit cannot lose its new queue head. */
    reset(); offer(1); replace_generation_on_write = true;
    usb_report_work_cb(&usb_report_work); assert(report_queue.count == 1);
    complete(); drain(); assert(writes == 2 && sent[1].data[1] == 77);

    reset(); offer(1); usb_endpoint_lock = 1;
    usb_report_work_cb(&usb_report_work); assert(writes == 0 && hid_sem == 1 && delay_ms == 1);
    usb_endpoint_lock = 0; drain(); assert(writes == 1);
    reset(); usb_status_cb(USB_DC_SUSPEND, NULL);
    unsigned int previous_scheduled = scheduled;
    usb_report_work_cb(&usb_report_work);
    assert(wakes == 0 && writes == 0 && scheduled == previous_scheduled);
    puts("14 actual USB pipeline scenarios passed");
    return 0;
}
