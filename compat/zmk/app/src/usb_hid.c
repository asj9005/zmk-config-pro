/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <totem/hid_report_queue.h>
#include <totem/esb_diagnostics.h>
#include <totem/field_diagnostics.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *hid_dev;

static K_SEM_DEFINE(hid_sem, 1, 1);
static struct totem_hid_queue report_queue;
static struct k_spinlock report_queue_lock;
/* nrfx USB status/protocol callbacks run in thread context. Serialize them
 * with endpoint submission, which may itself take the driver's mutex. Input
 * producers take only report_queue_lock and never wait on this mutex. */
static K_MUTEX_DEFINE(usb_endpoint_lock);
static struct totem_hid_packet in_flight_packet;
static bool in_flight;
static bool reports_need_resync;
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
static uint64_t field_submitted_at_us;
static bool field_submit_committed;
static bool field_completion_early;
#endif
static void usb_report_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(usb_report_work, usb_report_work_cb);

static void in_ready_cb(const struct device *dev) {
    ARG_UNUSED(dev);
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
    uint64_t completed_at_us = totem_field_now_us();
#endif
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
    if (in_flight) {
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
        if (field_submit_committed) {
            totem_field_observe(TOTEM_FIELD_USB_TRANSFER, completed_at_us - field_submitted_at_us);
            field_submit_committed = false;
        } else {
            field_completion_early = true;
        }
#endif
        in_flight = false;
        k_sem_give(&hid_sem);
    }
    k_spin_unlock(&report_queue_lock, key);
    k_work_reschedule(&usb_report_work, K_NO_WAIT);
}

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_OUTPUT 0x200
#define HID_REPORT_TYPE_FEATURE 0x300

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
static uint8_t hid_protocol = HID_PROTOCOL_REPORT;

void zmk_usb_hid_set_protocol(uint8_t protocol) {
    k_mutex_lock(&usb_endpoint_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
    if (hid_protocol != protocol) {
        hid_protocol = protocol;
        totem_hid_queue_reset(&report_queue);
        reports_need_resync = true;
    }
    k_spin_unlock(&report_queue_lock, key);
    k_mutex_unlock(&usb_endpoint_lock);
    k_work_reschedule(&usb_report_work, K_NO_WAIT);
}

static void set_proto_cb(const struct device *dev, uint8_t protocol) {
    ARG_UNUSED(dev);
    zmk_usb_hid_set_protocol(protocol);
}
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

static uint8_t *get_keyboard_report(size_t *len) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol != HID_PROTOCOL_REPORT) {
        zmk_hid_boot_report_t *boot_report = zmk_hid_get_boot_report();
        *len = sizeof(*boot_report);
        return (uint8_t *)boot_report;
    }
#endif
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    *len = sizeof(*report);
    return (uint8_t *)report;
}

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            static struct zmk_hid_mouse_resolution_feature_report res_feature_report;

            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            *len = sizeof(struct zmk_hid_mouse_resolution_feature_report);
            struct zmk_pointing_resolution_multipliers mult =
                zmk_pointing_resolution_multipliers_get_profile(endpoint);

            res_feature_report.body.wheel_res = mult.wheel;
            res_feature_report.body.hwheel_res = mult.hor_wheel;
            *data = (uint8_t *)&res_feature_report;
            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;
    case HID_REPORT_TYPE_INPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
        case ZMK_HID_REPORT_ID_KEYBOARD: {
            size_t size;
            *data = get_keyboard_report(&size);
            *len = (int32_t)size;
            break;
        }
        case ZMK_HID_REPORT_ID_CONSUMER: {
            struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
            *data = (uint8_t *)report;
            *len = sizeof(*report);
            break;
        }
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        /*
         * 7.2.1 of the HID v1.11 spec is unclear about handling requests for reports that do not
         * exist For requested reports that aren't input reports, return -ENOTSUP like the Zephyr
         * subsys does
         */
        LOG_ERR("Unsupported report type %d requested", (setup->wValue & HID_GET_REPORT_TYPE_MASK)
                                                            << 8);
        return -ENOTSUP;
    }

    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            if (*len != sizeof(struct zmk_hid_mouse_resolution_feature_report)) {
                return -EINVAL;
            }

            struct zmk_hid_mouse_resolution_feature_report *report =
                (struct zmk_hid_mouse_resolution_feature_report *)*data;
            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            zmk_pointing_resolution_multipliers_process_report(&report->body, endpoint);

            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;

    case HID_REPORT_TYPE_OUTPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        case ZMK_HID_REPORT_ID_LEDS:
            if (*len != sizeof(struct zmk_hid_led_report)) {
                LOG_ERR("LED set report is malformed: length=%d", *len);
                return -EINVAL;
            } else {
                struct zmk_hid_led_report *report = (struct zmk_hid_led_report *)*data;
                struct zmk_endpoint_instance endpoint = {
                    .transport = ZMK_TRANSPORT_USB,
                };
                zmk_hid_indicators_process_report(&report->body, endpoint);
            }
            break;
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        LOG_ERR("Unsupported report type %d requested",
                (setup->wValue & HID_GET_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }

    return 0;
}

static const struct hid_ops ops = {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    .protocol_change = set_proto_cb,
#endif
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

/* Called synchronously from usb_status_cb, after the nrfx driver has stopped
 * aborted transfers. The deferred connection event can coalesce RESET away. */
void totem_usb_hid_status_changed(enum usb_dc_status_code status) {
    if (status == USB_DC_RESET || status == USB_DC_DISCONNECTED) {
        /* Bounded reason code: 0 reset, 1 disconnected. Never report HID data. */
        totem_field_issue(TOTEM_FIELD_USB_RESET, TOTEM_FIELD_SCOPE_LOCAL,
                          status == USB_DC_DISCONNECTED ? 1 : 0);
    } else if (status == USB_DC_SUSPEND) {
        totem_field_issue(TOTEM_FIELD_USB_SUSPEND, TOTEM_FIELD_SCOPE_LOCAL, 0);
    } else if (status == USB_DC_RESUME) {
        totem_field_issue(TOTEM_FIELD_USB_RESUME, TOTEM_FIELD_SCOPE_LOCAL, 0);
    }
    if (status == USB_DC_RESET || status == USB_DC_DISCONNECTED) {
        k_mutex_lock(&usb_endpoint_lock, K_FOREVER);
        k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
        totem_hid_queue_reset(&report_queue);
        reports_need_resync = true;
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
        if (in_flight && field_submit_committed) {
            /* Timing discard codes: 1 completion before submit returned,
             * 2 accepted transfer aborted by reset/disconnect. */
            totem_field_issue(TOTEM_FIELD_USB_TIMING_DISCARD, TOTEM_FIELD_SCOPE_LOCAL, 2);
        }
        field_submit_committed = field_completion_early = false;
#endif
        in_flight = false;
        /* These aborts do not call int_in_ready. nrfx's ordered status and
         * completion callbacks cannot complete this old DMA after reset. */
        k_sem_give(&hid_sem);
        k_spin_unlock(&report_queue_lock, key);
        k_mutex_unlock(&usb_endpoint_lock);
    }
    k_work_reschedule(&usb_report_work, K_NO_WAIT);
}

static void usb_queue_current_reports_locked(void) {
    size_t len;
    uint8_t *keyboard = get_keyboard_report(&len);
    (void)totem_hid_queue_offer_resync(&report_queue, TOTEM_HID_KEYBOARD, keyboard, (uint8_t)len);
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return;
    }
#endif
    struct zmk_hid_consumer_report *consumer = zmk_hid_get_consumer_report();
    (void)totem_hid_queue_offer_resync(&report_queue, TOTEM_HID_CONSUMER, (uint8_t *)consumer,
                                     sizeof(*consumer));
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    struct zmk_hid_mouse_report mouse = *zmk_hid_get_mouse_report();
    mouse.body.d_x = mouse.body.d_y = 0;
    mouse.body.d_scroll_x = mouse.body.d_scroll_y = 0;
    (void)totem_hid_queue_offer_resync(&report_queue, TOTEM_HID_MOUSE, (uint8_t *)&mouse, sizeof(mouse));
#endif
}

static void usb_report_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
    bool pending = reports_need_resync || totem_hid_queue_pending(&report_queue);
    k_spin_unlock(&report_queue_lock, key);
    if (!pending) {
        return;
    }
    /* Never block the shared worker behind a USB status/control callback. */
    if (k_mutex_lock(&usb_endpoint_lock, K_NO_WAIT) != 0) {
        k_work_reschedule(&usb_report_work, K_MSEC(1));
        return;
    }
    if (zmk_usb_get_status() == USB_DC_SUSPEND) {
        (void)usb_wakeup_request();
        k_mutex_unlock(&usb_endpoint_lock);
        k_work_reschedule(&usb_report_work, K_MSEC(10));
        return;
    }
    if (!zmk_usb_is_hid_ready()) {
        k_mutex_unlock(&usb_endpoint_lock);
        return;
    }
    if (k_sem_take(&hid_sem, K_NO_WAIT) != 0) {
        k_mutex_unlock(&usb_endpoint_lock);
        k_work_reschedule(&usb_report_work, K_MSEC(1));
        return;
    }
    key = k_spin_lock(&report_queue_lock);
    if (reports_need_resync) {
        reports_need_resync = false;
        usb_queue_current_reports_locked();
    }
    pending = totem_hid_queue_peek(&report_queue, &in_flight_packet);
    uint32_t generation = report_queue.generation;
    in_flight = pending;
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
    field_submit_committed = field_completion_early = false;
#endif
    k_spin_unlock(&report_queue_lock, key);
    if (!pending) {
        k_sem_give(&hid_sem);
        k_mutex_unlock(&usb_endpoint_lock);
        return;
    }
    /* The endpoint retains this pointer until int_in_ready or an abort.
     * Producer snapshots and queue resets never modify the DMA buffer. */
    int err = hid_int_ep_write(hid_dev, in_flight_packet.data, in_flight_packet.length, NULL);
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
    uint64_t submitted_at_us = totem_field_now_us();
#endif
    key = k_spin_lock(&report_queue_lock);
    if (err != 0) {
        in_flight = false;
        k_sem_give(&hid_sem);
        k_spin_unlock(&report_queue_lock, key);
        k_mutex_unlock(&usb_endpoint_lock);
        totem_esb_diag_event(TOTEM_DIAG_USB_RETRY, err);
        totem_field_issue(TOTEM_FIELD_USB_RETRY, TOTEM_FIELD_SCOPE_LOCAL, err);
        k_work_reschedule(&usb_report_work, K_MSEC(1));
        return;
    }
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
    /* The API return is the observable successful-submit boundary, not host
     * receipt. An earlier ready callback cannot yield a valid transfer interval. */
    enum totem_field_metric metric = in_flight_packet.origin == TOTEM_HID_RECOVERY
        ? TOTEM_FIELD_USB_QUEUE_RECOVERY : in_flight_packet.origin == TOTEM_HID_RESYNC
        ? TOTEM_FIELD_USB_QUEUE_RESYNC : TOTEM_FIELD_USB_QUEUE;
    totem_field_observe(metric, submitted_at_us - in_flight_packet.queued_at_us);
    if (field_completion_early) {
        totem_field_issue(TOTEM_FIELD_USB_TIMING_DISCARD, TOTEM_FIELD_SCOPE_LOCAL, 1);
    } else if (in_flight) {
        field_submitted_at_us = submitted_at_us;
        field_submit_committed = true;
    }
#endif
    if (generation == report_queue.generation) {
        totem_hid_queue_pop(&report_queue);
    }
    pending = totem_hid_queue_pending(&report_queue);
    k_spin_unlock(&report_queue_lock, key);
    k_mutex_unlock(&usb_endpoint_lock);
    if (pending) {
        k_work_reschedule(&usb_report_work, K_NO_WAIT);
    }
}

static int zmk_usb_hid_queue_report(enum totem_hid_kind kind, const uint8_t *report,
                                  const uint8_t *recovery, size_t len) {
    /* Caller holds report_queue_lock across protocol selection and snapshot. */
    if (len == 0 || len > TOTEM_HID_REPORT_BYTES) {
        return -EMSGSIZE;
    }
    int result = totem_hid_queue_offer(&report_queue, kind, report, recovery, (uint8_t)len);
    uint32_t used = report_queue.count;
    if (result < 0) {
        return -EINVAL;
    }
    if (result > 0) {
        totem_esb_diag_event(TOTEM_DIAG_USB_OVERFLOW, 0);
#if defined(CONFIG_TOTEM_FIELD_DIAGNOSTICS)
        /* Bounded pressure snapshot: status[23:16], queued reports[15:8],
         * in-flight[0], resync pending[1]. No report bytes or input identity. */
        uint32_t context = (((uint32_t)zmk_usb_get_status() & 0xffU) << 16) |
                           ((used & 0xffU) << 8) | (in_flight ? 1U : 0U) |
                           (reports_need_resync ? 2U : 0U);
        totem_field_issue(TOTEM_FIELD_USB_OVERFLOW, TOTEM_FIELD_SCOPE_LOCAL, (int32_t)context);
#endif
    }
    totem_esb_diag_usb_observe(used);
    k_work_reschedule(&usb_report_work, K_NO_WAIT);
    return 0;
}

int zmk_usb_hid_send_keyboard_report(void) {
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
    size_t len;
    uint8_t *report = get_keyboard_report(&len);
    int err = zmk_usb_hid_queue_report(TOTEM_HID_KEYBOARD, report, report, len);
    k_spin_unlock(&report_queue_lock, key);
    return err;
}

int zmk_usb_hid_send_consumer_report(void) {
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        k_spin_unlock(&report_queue_lock, key);
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    int err = zmk_usb_hid_queue_report(TOTEM_HID_CONSUMER, (uint8_t *)report,
                                     (uint8_t *)report, sizeof(*report));
    k_spin_unlock(&report_queue_lock, key);
    return err;
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report(void) {
    k_spinlock_key_t key = k_spin_lock(&report_queue_lock);
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        k_spin_unlock(&report_queue_lock, key);
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    struct zmk_hid_mouse_report recovery = *report;
    /* Overflow recovery restores buttons, never repeats a stale relative move. */
    recovery.body.d_x = recovery.body.d_y = 0;
    recovery.body.d_scroll_x = recovery.body.d_scroll_y = 0;
    int err = zmk_usb_hid_queue_report(TOTEM_HID_MOUSE, (uint8_t *)report,
                                     (uint8_t *)&recovery, sizeof(*report));
    k_spin_unlock(&report_queue_lock, key);
    return err;
}
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static int zmk_usb_hid_init(void) {
    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    usb_hid_set_proto_code(hid_dev, HID_BOOT_IFACE_CODE_KEYBOARD);
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    usb_hid_init(hid_dev);

    return 0;
}

SYS_INIT(zmk_usb_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
