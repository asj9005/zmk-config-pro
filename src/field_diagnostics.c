/* SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <zmk/usb.h>
#include <esb.h>
#include <totem/esb_diagnostics.h>
#include <totem/field_diagnostics.h>
#include <totem/field_stats.h>
#include <totem/field_build_info.h>

#define FIELD_PARTS 72U
#define FIELD_LINE_BYTES 768U
#define FIELD_TX_CHUNK 64U
#define FIELD_TX_BUDGET_MS 1000U

static const char *const metric_names[] = {
    "rx_queue", "rx_process", "hold_tap_base_e", "hold_tap_base_r",
    "hold_tap_mouse_fast", "hold_tap_mouse_slow", "hold_tap_other", "timer_late",
    "usb_queue", "usb_queue_recovery", "usb_queue_resync", "usb_transfer",
};
static const char *const reason_names[] = {
    "peer_timeout", "auth_restart", "session_established", "handshake_timeout",
    "tx_queue_full", "presession_overflow", "rx_overflow", "scan_overflow",
    "usb_overflow", "usb_retry", "input_overflow", "hold_tap_overflow",
    "usb_reset", "usb_suspend", "usb_resume", "usb_timing_discard",
};
static const char *const group_names[] = {"base_e", "base_r", "mouse_fast", "mouse_slow", "other"};
BUILD_ASSERT(ARRAY_SIZE(metric_names) == TOTEM_FIELD_METRIC_COUNT);
BUILD_ASSERT(ARRAY_SIZE(reason_names) == TOTEM_FIELD_REASON_COUNT);
BUILD_ASSERT(ARRAY_SIZE(group_names) == TOTEM_FIELD_GROUP_COUNT);
BUILD_ASSERT(FIELD_PARTS == 2 + TOTEM_FIELD_METRIC_COUNT +
             TOTEM_FIELD_REASON_COUNT * TOTEM_FIELD_SCOPE_COUNT + TOTEM_FIELD_GROUP_COUNT + 5);

#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const char field_role[] = "dongle";
#elif CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID == 1
static const char field_role[] = "left";
#else
static const char field_role[] = "right";
#endif

struct field_issue {
    uint64_t last_ms;
    uint32_t count;
    int32_t code;
};
struct field_aggregates {
    struct totem_field_stats metrics[TOTEM_FIELD_METRIC_COUNT];
    struct field_issue issues[TOTEM_FIELD_REASON_COUNT][TOTEM_FIELD_SCOPE_COUNT];
    uint32_t holds[TOTEM_FIELD_GROUP_COUNT][2];
};
static struct field_aggregates live;
static struct k_spinlock field_lock;
static struct {
    struct field_aggregates stats;
    struct totem_esb_diag_snapshot diag;
    struct esb_diagnostics radio;
    bool radio_available;
    uint64_t uptime_ms;
    uint32_t dropped;
} snapshot;

uint64_t totem_field_now_us(void) {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

void totem_field_observe(enum totem_field_metric metric, uint64_t duration_us) {
    if ((unsigned int)metric >= TOTEM_FIELD_METRIC_COUNT) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&field_lock);
    (void)totem_field_stats_observe(&live.metrics[metric], duration_us);
    k_spin_unlock(&field_lock, key);
}

void totem_field_issue(enum totem_field_reason reason, uint8_t scope, int32_t code) {
    if ((unsigned int)reason >= TOTEM_FIELD_REASON_COUNT || scope >= TOTEM_FIELD_SCOPE_COUNT) {
        return;
    }
    uint64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&field_lock);
    struct field_issue *issue = &live.issues[reason][scope];
    issue->count = totem_field_sat_inc(issue->count);
    issue->last_ms = now;
    issue->code = code;
    k_spin_unlock(&field_lock, key);
}

void totem_field_hold_tap(enum totem_field_group group, bool hold) {
    if ((unsigned int)group >= TOTEM_FIELD_GROUP_COUNT) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&field_lock);
    live.holds[group][hold] = totem_field_sat_inc(live.holds[group][hold]);
    k_spin_unlock(&field_lock, key);
}

static struct k_work_q field_queue;
static K_THREAD_STACK_DEFINE(field_stack, 2048);
static const struct device *field_uart;
static uint32_t boot_high, boot_low;
static bool boot_initialized;
static struct totem_field_output_state output;
static uint64_t next_snapshot_ms;
static unsigned int part, line_length, line_offset;
static char line[FIELD_LINE_BYTES];
BUILD_ASSERT(sizeof(live) + sizeof(snapshot) + sizeof(field_stack) + sizeof(line) <= 8192,
             "Field diagnostics buffers and stack exceed their static RAM budget");

static void capture_snapshot(void) {
    snapshot.dropped = output.dropped;
    /* Short, separate critical sections: never copy the full snapshot with
     * interrupts masked. Entries can observe slightly different instants. */
    for (unsigned int i = 0; i < TOTEM_FIELD_METRIC_COUNT; i++) {
        k_spinlock_key_t key = k_spin_lock(&field_lock);
        snapshot.stats.metrics[i] = live.metrics[i];
        k_spin_unlock(&field_lock, key);
    }
    for (unsigned int i = 0; i < TOTEM_FIELD_REASON_COUNT; i++) {
        for (unsigned int scope = 0; scope < TOTEM_FIELD_SCOPE_COUNT; scope++) {
            k_spinlock_key_t key = k_spin_lock(&field_lock);
            snapshot.stats.issues[i][scope] = live.issues[i][scope];
            k_spin_unlock(&field_lock, key);
        }
    }
    for (unsigned int i = 0; i < TOTEM_FIELD_GROUP_COUNT; i++) {
        k_spinlock_key_t key = k_spin_lock(&field_lock);
        memcpy(snapshot.stats.holds[i], live.holds[i], sizeof(live.holds[i]));
        k_spin_unlock(&field_lock, key);
    }
    totem_esb_diag_snapshot(&snapshot.diag);
    memset(&snapshot.radio, 0, sizeof(snapshot.radio));
    snapshot.radio_available = esb_get_diagnostics(&snapshot.radio) == 0;
    snapshot.uptime_ms = k_uptime_get();
}

static bool append(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int length = vsnprintk(line + line_length, sizeof(line) - line_length, format, args);
    va_end(args);
    if (length < 0 || (unsigned int)length >= sizeof(line) - line_length) {
        return false;
    }
    line_length += (unsigned int)length;
    return true;
}

static bool format_diag(unsigned int index) {
    const struct totem_esb_diag_snapshot *d = &snapshot.diag;
    static const char *const stage[] = {"crypto_init", "crypto_kat", "crypto_roots", "boot_nonce", "clock", "radio", "transport"};
    static const char *const events[] = {"tx_ok", "tx_fail", "rx", "frame_ok", "frame_err", "hello", "challenge", "ready", "session_ok", "rx_drop", "bad_position", "ht_overflow", "scan_overflow", "scan_resync", "usb_retry", "usb_overflow", "input_retry", "input_overflow"};
    static const char *const steps[] = {"send", "write", "start", "hf_request", "hf_callback", "hf_wait", "radio_busy"};
    if (index == 0) {
        if (!append("kind=diag name=startup")) return false;
        for (unsigned int i = 0; i < TOTEM_DIAG_STAGE_COUNT; i++) {
            if (!append(" %s=%d", stage[i], d->stages[i])) return false;
        }
        return append(" kat_step=%d kat_status=%d", d->kat_step, d->kat_status);
    }
    if (index == 1) {
        if (!append("kind=diag name=events")) return false;
        for (unsigned int i = 0; i < TOTEM_DIAG_EVENT_COUNT; i++) {
            if (!append(" %s=%u", events[i], d->events[i])) return false;
        }
        return append(" last_frame_err=%d", d->frame_error);
    }
    if (index == 2) {
        if (!append("kind=diag name=tx")) return false;
        for (unsigned int i = 0; i < TOTEM_DIAG_TX_STEP_COUNT; i++) {
            if (!append(" %s_n=%u %s_rc=%d", steps[i], d->tx_counts[i], steps[i], d->tx_results[i])) return false;
        }
        return true;
    }
    if (index == 3) {
        const struct esb_diagnostics *r = &snapshot.radio;
        return append("kind=diag name=radio available=%u sdk_state=%u radio_state=%u tx_queued=%u retries=%u irq_flags=%u radio_events=%u timer_events=%u timer_shorts=%u radio_irq=%u timer_irq=%u late_ack=%u",
            (unsigned int)snapshot.radio_available, r->state, r->radio_state, r->tx_queued,
            r->retries, r->irq_flags, r->radio_events, r->timer_events, r->timer_shorts,
            r->radio_irq, r->timer_irq, r->late_ack_setup);
    }
    return append("kind=diag name=high rx_high=%u rx_age_max=%u scan_high=%u usb_high=%u",
                  d->rx_high, d->rx_age_max, d->scan_high, d->usb_high);
}

static bool format_part(void) {
    line_length = line_offset = 0;
    /* A fresh newline also terminates a partial frame left by disconnect. */
    if (!append("%s[totem-field] schema=1 role=%s boot=%08x%08x seq=%u part=%u total=%u ",
                part == 0 ? "\n" : "", field_role, boot_high, boot_low,
                output.sequence, part, FIELD_PARTS)) return false;
    if (part == 0) {
        if (!append("kind=begin format=TOTEM_FIELD_V1 fw=%s tree=%s dirty=%u uptime_ms=%llu tick_hz=%u rx_queue_resolution_us=1000 interval_ms=%u tx_drop=%u",
                    TOTEM_FIELD_BUILD_SHA, TOTEM_FIELD_BUILD_TREE, TOTEM_FIELD_BUILD_DIRTY,
                    (unsigned long long)snapshot.uptime_ms, CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    CONFIG_TOTEM_FIELD_INTERVAL_MS, snapshot.dropped)) return false;
    } else if (part <= TOTEM_FIELD_METRIC_COUNT) {
        unsigned int index = part - 1U;
        const struct totem_field_stats *s = &snapshot.stats.metrics[index];
        if (!append("kind=metric name=%s count=%u sum_us=%llu max_us=%u flags=%u",
                    metric_names[index], s->count, (unsigned long long)s->sum_us, s->max_us, s->flags)) return false;
        for (unsigned int bin = 0; bin < TOTEM_FIELD_BINS; bin++) {
            if (!append(" b%u=%u", bin, s->bins[bin])) return false;
        }
    } else if (part < 61U) {
        unsigned int index = part - 1U - TOTEM_FIELD_METRIC_COUNT;
        unsigned int reason = index / TOTEM_FIELD_SCOPE_COUNT;
        unsigned int scope = index % TOTEM_FIELD_SCOPE_COUNT;
        const struct field_issue *i = &snapshot.stats.issues[reason][scope];
        if (!append("kind=issue name=%s scope=%u count=%u last_ms=%llu code=%d",
                    reason_names[reason], scope, i->count, (unsigned long long)i->last_ms, i->code)) return false;
    } else if (part < 66U) {
        unsigned int group = part - 61U;
        if (!append("kind=holdtap name=%s tap=%u hold=%u", group_names[group],
                    snapshot.stats.holds[group][0], snapshot.stats.holds[group][1])) return false;
    } else if (part < 71U) {
        if (!format_diag(part - 66U)) return false;
    } else {
        if (!append("kind=end uptime_ms=%llu tx_drop=%u",
                    (unsigned long long)snapshot.uptime_ms, snapshot.dropped)) return false;
    }
    return append("\n");
}

static void field_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(field_work, field_work_handler);

static bool field_connected(void) {
    /* Available in both central and logging-only USB halves on pinned ZMK. */
    return zmk_usb_get_status() != USB_DC_SUSPEND && zmk_usb_is_hid_ready();
}

static void schedule_next(void) {
    uint64_t now = k_uptime_get();
    uint64_t delay = output.active ? 1U : next_snapshot_ms > now ? next_snapshot_ms - now : 1U;
    k_work_reschedule_for_queue(&field_queue, &field_work, K_MSEC(delay));
}

static void field_work_handler(struct k_work *work) {
    (void)work;
    uint64_t now = k_uptime_get();
    if (!boot_initialized) {
        /* Public random identifier only; never reuse a protocol nonce/session. */
        boot_high = sys_rand32_get();
        boot_low = sys_rand32_get();
        boot_initialized = true;
    }
    if (!output.active) {
        totem_field_output_begin(&output, now, FIELD_TX_BUDGET_MS);
        next_snapshot_ms = now + CONFIG_TOTEM_FIELD_INTERVAL_MS;
        part = line_length = line_offset = 0;
        if (field_connected()) {
            capture_snapshot();
        }
    }
    if (!totem_field_output_ready(&output, now, field_connected())) {
        schedule_next();
        return;
    }
    if (line_offset == line_length && !format_part()) {
        totem_field_output_abort(&output);
        schedule_next();
        return;
    }
    unsigned int requested = MIN(FIELD_TX_CHUNK, line_length - line_offset);
    /* Pinned Zephyr cdc_acm_fifo_fill masks IRQ only for ring_buf_put, returns
     * accepted bytes immediately and schedules USB work. Do not use poll_out,
     * wait for DTR, or install/replace the console's IRQ callback. */
    int accepted = uart_fifo_fill(field_uart, (uint8_t *)line + line_offset, requested);
    if (totem_field_output_accept(&output, requested, accepted)) {
        line_offset += (unsigned int)accepted;
        if (line_offset == line_length) {
            part++;
            if (part == FIELD_PARTS) {
                output.active = false;
            }
        }
    }
    schedule_next();
}

static int field_init(void) {
#if DT_HAS_CHOSEN(zephyr_console)
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)
    field_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(field_uart)) return 0;
    k_work_queue_start(&field_queue, field_stack, K_THREAD_STACK_SIZEOF(field_stack),
                       CONFIG_TOTEM_FIELD_THREAD_PRIORITY, NULL);
    k_work_reschedule_for_queue(&field_queue, &field_work, K_MSEC(1000));
#endif
#endif
    return 0;
}
SYS_INIT(field_init, APPLICATION, 99);
