/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A controlled failure exits without invoking Windows crash reporting. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed line %d: %s\n", __LINE__, #condition); exit(87); \
} } while (0)

#define IS_ENABLED(x) (x)
#define CONFIG_ZMK_SPLIT 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT 2
#define CONFIG_ZMK_USB 1
#define ZMK_KEYMAP_LEN 38
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL UINT8_MAX
#define K_FOREVER -1
#define K_MSEC(ms) (ms)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_EV_EVENT_BUBBLE 0
#define HID_USAGE_KEY 7
#define TAB 43
#define ZMK_HID_USAGE_ID(x) (x)
#define MOD_LALT 4
#define MOD_LCTL 1
#define MOD_LSFT 2
#define MOD_LGUI 8
#define MOD_RCTL 16
#define MOD_RSFT 32
#define MOD_RALT 64
#define MOD_RGUI 128
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)

struct k_work { int unused; };
struct k_work_delayable { struct k_work work; void (*callback)(struct k_work *); };
#define K_WORK_DELAYABLE_DEFINE(name, cb) struct k_work_delayable name = {.callback = cb}
#define K_MUTEX_DEFINE(name) int name
static int lock_depth;
static int k_mutex_lock(int *mutex, int timeout) { (void)mutex; (void)timeout; lock_depth++; return 0; }
static int k_mutex_unlock(int *mutex) { (void)mutex; assert(lock_depth > 0); lock_depth--; return 0; }
static int64_t now;
static int64_t scheduled_at;
static bool queued;
static bool callback_already_submitted;
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    (void)work; queued = false;
    return callback_already_submitted ? -EINPROGRESS : 0;
}
static int k_work_reschedule(struct k_work_delayable *work, int64_t delay) {
    (void)work; scheduled_at = now + delay; queued = true; return 1;
}
static int64_t k_uptime_get(void) { return now; }

struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1; uint32_t param2; };
struct zmk_behavior_binding_event { uint32_t position; uint8_t source; int64_t timestamp; };
struct zmk_position_state_changed { uint8_t source; uint32_t position; bool state; int64_t timestamp; };
struct zmk_layer_state_changed { uint8_t layer; bool state; };
struct device { const void *config; };
static struct device device;
static const struct device *zmk_behavior_get_binding(const char *name) { (void)name; return &device; }

/* Exact pinned modifier reference-count functions are injected here. */
typedef uint8_t zmk_mod_t;
typedef uint8_t zmk_mod_flags_t;
static int explicit_modifier_counts[8];
static zmk_mod_flags_t explicit_modifiers;
static zmk_mod_flags_t report_modifiers;
#define BIT(i) (1U << (i))
#define WRITE_BIT(value, bit, set) do { if (set) value |= BIT(bit); else value &= ~BIT(bit); } while (0)
#define GET_MODIFIERS report_modifiers
#define SET_MODIFIERS(mods) do { report_modifiers = mods; } while (0)
/* PINNED_MODIFIER_FUNCTIONS */

/* Compile the real widget snapshot getter, including Caps Word retention.
 * The event/widget workqueue itself is deliberately a deterministic fake. */
#define CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED 1
enum { MOD_TYPE_GUI, MOD_TYPE_ALT, MOD_TYPE_CTRL, MOD_TYPE_SHIFT };
struct zmk_caps_word_state_changed { bool active; };
typedef struct { int kind; struct zmk_caps_word_state_changed caps; } zmk_event_t;
static const struct zmk_caps_word_state_changed *as_zmk_caps_word_state_changed(const zmk_event_t *e) {
    return e->kind == 3 ? &e->caps : NULL;
}
static zmk_mod_flags_t zmk_hid_get_explicit_mods(void) { return explicit_modifiers; }
/* ACTUAL_MODIFIER_WIDGET_GETTER */
static struct modifier_indicator_state displayed_modifiers;
static unsigned int modifier_notifications;
static bool fail_modifier_notification;
struct zmk_modifiers_state_changed { zmk_mod_flags_t modifiers; bool state; };
static int raise_zmk_modifiers_state_changed(struct zmk_modifiers_state_changed event) {
    assert(event.modifiers == MOD_LALT);
    assert(event.state == ((explicit_modifiers & MOD_LALT) != 0));
    modifier_notifications++;
    const zmk_event_t display_event = {2, {false}};
    displayed_modifiers = modifier_indicator_get_state(&display_event);
    return fail_modifier_notification ? -EIO : 0;
}

static bool tab_down;
static bool sticky_pending;
static bool sticky_consumed;
static bool fail_endpoint;
static int tab_press_error;  /* 1 before state mutation, 2 after */
static int tab_release_error;
static unsigned int send_calls;
static unsigned int tab_presses;
static unsigned int tab_releases;
static unsigned int tab_fallback_releases;
static bool sent_tab;
static uint8_t sent_mods;
enum { ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
enum { USB_DC_CONFIGURED, USB_DC_SUSPEND };
struct zmk_endpoint_instance { int transport; };
static int selected_transport = ZMK_TRANSPORT_USB;
static bool usb_suspended;
static bool resume_during_send;
static bool suspend_during_send;
static uint32_t advance_on_report_once;
static struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){selected_transport};
}
static int zmk_usb_get_status(void) { return usb_suspended ? USB_DC_SUSPEND : USB_DC_CONFIGURED; }
static int zmk_endpoint_send_report(uint16_t usage) {
    assert(usage == HID_USAGE_KEY); send_calls++;
    now += advance_on_report_once; advance_on_report_once = 0;
    if (fail_endpoint) return -EIO;
    if (selected_transport == ZMK_TRANSPORT_USB && usb_suspended) {
        if (resume_during_send) { usb_suspended = false; resume_during_send = false; }
        return 0; /* wakeup succeeded, no HID report sent */
    }
    if (suspend_during_send) { usb_suspended = true; suspend_during_send = false; return 0; }
    sent_tab = tab_down; sent_mods = report_modifiers; return 0;
}
static int zmk_hid_keyboard_release(unsigned int key) {
    assert(key == TAB); tab_down = false; tab_fallback_releases++; return 0;
}
static int raise_zmk_keycode_state_changed_from_encoded(unsigned int key, bool pressed, int64_t stamp) {
    (void)stamp; assert(key == TAB);
    int failure = pressed ? tab_press_error : tab_release_error;
    if (pressed) tab_presses++; else tab_releases++;
    if (failure == 1) return -EIO;
    tab_down = pressed;
    if (pressed && sticky_pending) sticky_consumed = true;
    if (!pressed && sticky_pending && sticky_consumed) {
        assert(zmk_hid_unregister_mods(MOD_LALT) >= 0);
        sticky_pending = false;
    }
    /* Match pinned hid_listener: endpoint error is swallowed by that listener. */
    const zmk_event_t display_event = {1, {false}};
    displayed_modifiers = modifier_indicator_get_state(&display_event);
    (void)zmk_endpoint_send_report(HID_USAGE_KEY);
    return failure == 2 ? -EIO : 0;
}
/* ACTUAL_CANDIDATE_FUNCTIONS */

static const uint32_t ignored[] = {2, 3, 7, 16, 17, 18};
static struct owned_swapper_config config = {ignored, 6};
static struct zmk_behavior_binding binding = {"swapper", 0, 0};
static struct zmk_behavior_binding_event handle(uint8_t source) {
    return (struct zmk_behavior_binding_event){12, source, now};
}
static void press(uint8_t source) { assert(owned_swapper_pressed(&binding, handle(source)) == 0); }
static void release(uint8_t source) { assert(owned_swapper_released(&binding, handle(source)) == 0); }
static void press_at(uint8_t source, uint32_t position, int64_t timestamp) {
    struct zmk_behavior_binding_event event = {position, source, timestamp};
    assert(owned_swapper_pressed(&binding, event) == 0);
}
static void release_at(uint8_t source, uint32_t position, int64_t timestamp) {
    struct zmk_behavior_binding_event event = {position, source, timestamp};
    assert(owned_swapper_released(&binding, event) == 0);
}
static void position(uint32_t pos, uint8_t source, bool state) {
    struct zmk_position_state_changed event = {source, pos, state, now};
    assert(owned_swapper_position(&event) == 0);
}
static void callback_at(int64_t time) {
    now = time; callback_already_submitted = false; queued = false;
    swapper_work_handler(&swapper_work.work);
    assert(lock_depth == 0);
}
static void reset(void) {
    memset(&swapper, 0, sizeof(swapper));
#if SWAPPER_REMOTE_SOURCE_COUNT > 0
    memset(source_reset_cutoff, 0, sizeof(source_reset_cutoff));
    memset(source_reset_valid, 0, sizeof(source_reset_valid));
#endif
    swapper.retry_ms = SWAPPER_RETRY_MIN_MS;
    memset(explicit_modifier_counts, 0, sizeof(explicit_modifier_counts));
    explicit_modifiers = 0; report_modifiers = 0; now = 0; scheduled_at = 0;
    caps_word_active = false;
    displayed_modifiers = modifier_indicator_get_state(NULL);
    modifier_notifications = 0; fail_modifier_notification = false;
    tab_down = false; sticky_pending = false; sticky_consumed = false; fail_endpoint = false;
    tab_press_error = 0; tab_release_error = 0; send_calls = 0; tab_presses = 0;
    tab_releases = 0; tab_fallback_releases = 0; sent_tab = false; sent_mods = 0;
    queued = false; callback_already_submitted = false;
    lock_depth = 0; device.config = &config;
    selected_transport = ZMK_TRANSPORT_USB; usb_suspended = false;
    resume_during_send = false; suspend_during_send = false;
    advance_on_report_once = 0;
}
static void no_owned_keys(void) {
    assert(!swapper.active && !swapper.alt_owned && !swapper.tab_pressed);
    assert(!tab_down); assert(explicit_modifier_counts[2] == 0);
}

/* Actual central cleanup function must cancel even an already-released latch,
 * and cancellation must occur after synthetic releases that resolve hold-taps. */
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 38
static uint8_t key_pos_states[2][5];
static bool synth_resolves_tap;
static int raise_zmk_position_state_changed(struct zmk_position_state_changed event) {
    owned_swapper_position(&event);
    if (event.position == 12 && !event.state && synth_resolves_tap) {
        press(event.source); release(event.source);
    }
    return 0;
}
/* ACTUAL_CENTRAL_CLEANUP */

/* Pinned queue-based end behavior negative control. */
static unsigned int legacy_queue_calls;
static bool behavior_queue_full;
struct legacy_config { struct zmk_behavior_binding end_behavior; int tap_ms; };
struct active_tri_state { uint32_t position; uint8_t source; const struct legacy_config *config; };
static int zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                                 struct zmk_behavior_binding b, bool down, uint32_t wait) {
    (void)event; (void)b; (void)down; (void)wait; legacy_queue_calls++;
    return behavior_queue_full ? -ENOSPC : 0;
}
/* PINNED_LEGACY_END */


int main(void) {
    unsigned int cases = 0;
    reset(); press(0); assert(tab_down && explicit_modifier_counts[2] == 1);
    release(0); assert(!tab_down && swapper.active && explicit_modifier_counts[2] == 1);
    position(14, 0, true); no_owned_keys(); cases++;

    reset(); press(0); release(0); position(14, 0, false); no_owned_keys(); cases++;

    reset(); press(0); press(0); assert(tab_presses == 1 && explicit_modifier_counts[2] == 1);
    release(0);
    unsigned int sends_after_release = send_calls;
    release(0); assert(tab_releases == 1 && send_calls == sends_after_release && !queued);
    position(14, 0, true); no_owned_keys(); cases++;

    /* A duplicate physical release must preserve an already pending report's
     * deadline and backoff instead of sending and scheduling it again. */
    reset(); press(0); fail_endpoint = true; release(0);
    assert(swapper.active && !swapper.pressed && !tab_down && swapper.report_pending && queued);
    sends_after_release = send_calls;
    int64_t retry_after_release = swapper.retry_at;
    int64_t scheduled_after_release = scheduled_at;
    uint32_t backoff_after_release = swapper.retry_ms;
    now++;
    release(0);
    assert(tab_releases == 1 && send_calls == sends_after_release);
    assert(swapper.report_pending && queued && swapper.retry_at == retry_after_release);
    assert(scheduled_at == scheduled_after_release && swapper.retry_ms == backoff_after_release);
    fail_endpoint = false; callback_at(scheduled_at);
    assert(!swapper.report_pending && !queued && !sent_tab && sent_mods == MOD_LALT);
    position(14, 0, true); no_owned_keys(); cases++;

    reset(); press(0); release(0);
    callback_at(5000); callback_at(60000); callback_at(3600000);
    assert(swapper.active && !queued && explicit_modifier_counts[2] == 1 && tab_presses == 1);
    position(14, 0, true); no_owned_keys(); cases++;

    reset(); press(0); callback_at(3600000);
    assert(swapper.active && swapper.pressed && tab_down && !queued);
    release(0); assert(swapper.active && !tab_down && !queued);
    totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); press(0); release(0);
    for (unsigned int i = 0; i < 6; i++) {
        position(ignored[i], 1, true); callback_at(now + 3600000); assert(swapper.active);
        position(ignored[i], 1, false); callback_at(now + 3600000); assert(swapper.active);
    }
    assert(!queued); position(19, 1, true); no_owned_keys(); cases++;

    reset(); position(7, 1, true); press(0); release(0); callback_at(3600000);
    assert(swapper.active && !queued); position(7, 1, false); callback_at(7200000);
    assert(swapper.active && !queued); totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); press(0); release(0);
    callback_already_submitted = true; position(14, 0, true); no_owned_keys();
    now = 900; press(0); release(0); callback_at(1000); callback_at(3600000);
    assert(swapper.active && explicit_modifier_counts[2] == 1);
    totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); press(0); release(0); totem_owned_swapper_source_reset(1);
    assert(swapper.active && explicit_modifier_counts[2] == 1);
    totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); press(0); release(0); memset(key_pos_states, 0, sizeof(key_pos_states));
    synth_resolves_tap = false; release_source_keys(0); no_owned_keys(); cases++;

    reset(); memset(key_pos_states, 0, sizeof(key_pos_states));
    key_pos_states[0][12 / 8] |= BIT(12 % 8); synth_resolves_tap = true;
    release_source_keys(0); assert(tab_presses == 1 && tab_releases == 1);
    no_owned_keys(); assert(key_pos_states[0][12 / 8] == 0); cases++;

    reset(); zmk_hid_register_mods(MOD_LALT); sticky_pending = true;
    press(0); assert(explicit_modifier_counts[2] == 2);
    release(0); assert(!sticky_pending && explicit_modifier_counts[2] == 1);
    position(14, 0, true); no_owned_keys(); cases++;

    reset(); zmk_hid_register_mods(MOD_LALT); press(0); release(0); position(14, 0, true);
    assert(explicit_modifier_counts[2] == 1 && !swapper.alt_owned && (sent_mods & MOD_LALT));
    zmk_hid_unregister_mods(MOD_LALT); no_owned_keys(); cases++;

    reset(); press(0); release(0); zmk_hid_register_mods(MOD_LALT);
    totem_owned_swapper_source_reset(0); assert(explicit_modifier_counts[2] == 1);
    zmk_hid_unregister_mods(MOD_LALT); no_owned_keys(); cases++;

    reset(); press(0); release(0); fail_endpoint = true; position(14, 0, true);
    no_owned_keys(); assert(swapper.report_pending);
    zmk_hid_register_mods(MOD_LALT);
    for (unsigned int i = 0; i < 8; i++) {
        int64_t when = scheduled_at; callback_at(when);
        assert(explicit_modifier_counts[2] == 1 && swapper.report_pending);
        assert(scheduled_at > when && scheduled_at - when <= 1000);
    }
    fail_endpoint = false; callback_at(scheduled_at);
    assert(!swapper.report_pending && explicit_modifier_counts[2] == 1 && (sent_mods & MOD_LALT));
    zmk_hid_unregister_mods(MOD_LALT); no_owned_keys(); cases++;

    reset(); press(0); release(0); fail_endpoint = true; position(14, 0, true);
    now = 10; fail_endpoint = false; press(0); callback_at(3600000);
    assert(swapper.pressed && tab_down && explicit_modifier_counts[2] == 1);
    release(0); totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    /* Another unresolved hold-tap may replay old D edges only after the ESB
     * cleanup call returned. They must not create a new, unreleasable latch. */
    reset(); now = 100; memset(key_pos_states, 0, sizeof(key_pos_states));
    synth_resolves_tap = false; release_source_keys(0);
    assert(source_reset_valid[0] && source_reset_cutoff[0] == 100);
    now = 1000; press_at(0, 12, 50); release_at(0, 12, 70);
    no_owned_keys(); assert(tab_presses == 0 && send_calls == 0); cases++;

    /* Equality is intentionally conservative, while the next millisecond is
     * accepted. A never-reset source still accepts timestamp zero. */
    reset(); now = 100; totem_owned_swapper_source_reset(0);
    press_at(0, 12, 100); no_owned_keys(); assert(tab_presses == 0);
    now = 101; press(0); release(0);
    assert(swapper.active && tab_presses == 1);
    position(14, 0, true); no_owned_keys();
    press_at(1, 12, 0); release_at(1, 12, 0); assert(swapper.active && swapper.source == 1);
    totem_owned_swapper_source_reset(1); no_owned_keys(); cases++;

    /* Cutoffs are per source, not a global disconnected-at timestamp. */
    reset(); now = 100; totem_owned_swapper_source_reset(0);
    now = 200; totem_owned_swapper_source_reset(1);
    now = 300; press_at(0, 12, 150); release_at(0, 12, 150);
    assert(swapper.active && swapper.source == 0);
    press_at(1, 13, 199);
    assert(swapper.active && swapper.source == 0 && swapper.position == 12 && tab_presses == 1);
    position(14, 0, true); no_owned_keys(); cases++;

    /* A stale activation may neither replace a new same-source latch nor
     * close one that is owned by the other half. */
    reset(); now = 100; totem_owned_swapper_source_reset(0);
    now = 200; press(0);
    press_at(0, 13, 50);
    assert(swapper.active && swapper.pressed && swapper.position == 12 && tab_down);
    assert(explicit_modifier_counts[2] == 1 && tab_presses == 1 && tab_releases == 0);
    release(0); position(14, 0, true); no_owned_keys();
    press(1); release(1); press_at(0, 12, 100);
    assert(swapper.active && swapper.source == 1 && tab_presses == 2);
    totem_owned_swapper_source_reset(1); no_owned_keys(); cases++;

    /* LOCAL and invalid sources never index the remote arrays. Reset calls
     * for them do not disturb an active remote or LOCAL swapper. */
    reset(); press(0); release(0);
    totem_owned_swapper_source_reset(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
    totem_owned_swapper_source_reset(SWAPPER_REMOTE_SOURCE_COUNT);
    totem_owned_swapper_source_reset(UINT8_MAX - 1);
    assert(swapper.active && !source_reset_valid[0] && !source_reset_valid[1]);
    press_at(SWAPPER_REMOTE_SOURCE_COUNT, 13, now);
    press_at(UINT8_MAX - 1, 13, now);
    assert(swapper.active && swapper.source == 0 && swapper.position == 12 && tab_presses == 1);
    position(14, 0, true); no_owned_keys();
    now = 100; totem_owned_swapper_source_reset(0); totem_owned_swapper_source_reset(1);
    press_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
    release_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
    totem_owned_swapper_source_reset(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
    assert(swapper.active && swapper.source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
    position(14, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true); no_owned_keys(); cases++;

    /* The boundary is recorded after synchronous cleanup, including time
     * spent sending its release report. */
    reset(); now = 90; press(0); release(0); now = 100;
    advance_on_report_once = 37; totem_owned_swapper_source_reset(0);
    assert(now == 137 && source_reset_cutoff[0] == 137 && source_reset_valid[0]);
    now = 200; press_at(0, 12, 130); no_owned_keys();
    press_at(0, 12, 138); release_at(0, 12, 138); assert(swapper.active);
    totem_owned_swapper_source_reset(0); no_owned_keys();
    assert(source_reset_cutoff[0] == 200);
    now = 300; press_at(0, 12, 150); no_owned_keys(); cases++;

    /* A stale nonignored position still ends an active latch. This can close
     * a newly opened switcher early, but cannot create stuck Alt. */
    reset(); now = 100; totem_owned_swapper_source_reset(0);
    now = 200; press(1); release(1);
    struct zmk_position_state_changed cutoff_replay = {0, 14, false, 50};
    totem_owned_swapper_replayed_position(&cutoff_replay); no_owned_keys(); cases++;

    for (int error = 1; error <= 2; error++) {
        reset(); tab_press_error = error;
        assert(owned_swapper_pressed(&binding, handle(0)) == -EIO);
        no_owned_keys(); cases++;
        reset(); press(0); tab_release_error = error; release(0);
        assert(!tab_down && swapper.active && tab_fallback_releases == 1);
        totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;
    }

    reset(); press(0); release(0); struct zmk_layer_state_changed layer = {8, false};
    owned_swapper_layer(&layer); assert(swapper.active);
    layer.state = true; owned_swapper_layer(&layer); no_owned_keys(); cases++;

    reset(); press(0); release(0); press(1);
    assert(swapper.source == 1 && swapper.pressed && explicit_modifier_counts[2] == 1);
    release(0); assert(tab_down); release(1); totem_owned_swapper_source_reset(1);
    no_owned_keys(); cases++;

    reset(); zmk_hid_register_mods(MOD_LALT);
    struct legacy_config legacy = {{"kt", 0, 0}, 5};
    struct active_tri_state legacy_state = {12, 0, &legacy};
    behavior_queue_full = true; legacy_queue_calls = 0;
    trigger_end_behavior(&legacy_state);
    assert(legacy_queue_calls == 2 && explicit_modifier_counts[2] == 1);
    reset(); legacy_queue_calls = 0; press(0); release(0); position(14, 0, true);
    no_owned_keys(); assert(legacy_queue_calls == 0); cases++;

    reset(); position(12, 0, true); position(14, 0, true); position(14, 0, false);
    press(0); /* hold-tap decides tap, then replays captured G events */
    struct zmk_position_state_changed replay = {0, 14, true, now};
    totem_owned_swapper_replayed_position(&replay); no_owned_keys();
    replay.state = false; totem_owned_swapper_replayed_position(&replay);
    release(0); no_owned_keys(); cases++;

    reset(); press(0); release(0);
    position(14, 0, true); /* combo consumes raw key; no replay follows */
    no_owned_keys(); cases++;

    reset(); position(7, 1, true); position(7, 1, false); press(0); release(0);
    replay = (struct zmk_position_state_changed){1, 7, true, now};
    totem_owned_swapper_replayed_position(&replay);
    replay.state = false; totem_owned_swapper_replayed_position(&replay);
    callback_at(3600000); assert(swapper.active && !queued);
    position(14, 0, true); no_owned_keys(); cases++;

    reset(); press(0); release(0); assert(sent_mods & MOD_LALT);
    usb_suspended = true; position(14, 0, true); no_owned_keys();
    assert(swapper.report_pending && (sent_mods & MOD_LALT));
    callback_at(scheduled_at); assert(swapper.report_pending && (sent_mods & MOD_LALT));
    usb_suspended = false; callback_at(scheduled_at);
    assert(!swapper.report_pending && !(sent_mods & MOD_LALT)); cases++;

    reset(); press(0); release(0); usb_suspended = true; resume_during_send = true;
    position(14, 0, true); no_owned_keys();
    assert(!usb_suspended && swapper.report_pending && (sent_mods & MOD_LALT));
    callback_at(scheduled_at); assert(!swapper.report_pending && !(sent_mods & MOD_LALT)); cases++;

    reset(); press(0); release(0); suspend_during_send = true;
    position(14, 0, true); no_owned_keys(); assert(swapper.report_pending && usb_suspended);
    usb_suspended = false; callback_at(scheduled_at); assert(!swapper.report_pending); cases++;

    reset(); selected_transport = ZMK_TRANSPORT_BLE; usb_suspended = true;
    press(0); release(0); position(14, 0, true); no_owned_keys();
    assert(!swapper.report_pending && !(sent_mods & MOD_LALT)); cases++;

    reset(); fail_endpoint = true; press(0);
    for (unsigned int i = 0; i < 10; i++) {
        callback_at(scheduled_at);
        assert(swapper.active && swapper.pressed && tab_down);
        assert(tab_presses == 1 && tab_releases == 0 && explicit_modifier_counts[2] == 1);
    }
    fail_endpoint = false; callback_at(scheduled_at);
    assert(!swapper.report_pending && swapper.active);
    release(0); totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); now = 100;
    struct zmk_behavior_binding_event old_release = handle(0);
    now = 200; totem_owned_swapper_source_reset(0);
    now = 201; press(0);
    assert(owned_swapper_released(&binding, old_release) == 0);
    assert(swapper.pressed && tab_down && tab_releases == 0);
    release(0); assert(!tab_down && tab_releases == 1);
    totem_owned_swapper_source_reset(0); no_owned_keys(); cases++;

    reset(); press(0); release(0);
    assert(displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 1);
    position(33, 0, false); no_owned_keys();
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 2);
    assert(tab_presses == 1 && tab_releases == 1); cases++;

    reset(); press(0); release(0);
    layer = (struct zmk_layer_state_changed){8, true};
    owned_swapper_layer(&layer); no_owned_keys();
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 2); cases++;

    reset(); press(0); release(0); totem_owned_swapper_source_reset(0); no_owned_keys();
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 2); cases++;

    reset(); zmk_hid_register_mods(MOD_LALT); press(0); release(0);
    totem_owned_swapper_source_reset(0);
    assert(displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 0);
    assert(explicit_modifier_counts[2] == 1); zmk_hid_unregister_mods(MOD_LALT); cases++;

    reset(); zmk_hid_register_mods(MOD_LALT); sticky_pending = true;
    press(0); release(0); position(33, 0, false); no_owned_keys();
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 1); cases++;

    reset(); const zmk_event_t caps_event = {3, {true}};
    displayed_modifiers = modifier_indicator_get_state(&caps_event);
    zmk_hid_register_mods(MOD_LCTL | MOD_RGUI | MOD_RSFT);
    press(0); release(0); position(33, 0, false);
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && displayed_modifiers.caps_word);
    assert(displayed_modifiers.mods[MOD_TYPE_CTRL] && displayed_modifiers.mods[MOD_TYPE_GUI]);
    assert(displayed_modifiers.mods[MOD_TYPE_SHIFT]);
    assert(explicit_modifier_counts[0] == 1 && explicit_modifier_counts[5] == 1);
    assert(explicit_modifier_counts[7] == 1); cases++;

    reset(); fail_modifier_notification = true; press(0); release(0);
    fail_endpoint = true; totem_owned_swapper_source_reset(0); no_owned_keys();
    assert(!displayed_modifiers.mods[MOD_TYPE_ALT] && modifier_notifications == 2);
    assert(swapper.report_pending); callback_at(scheduled_at);
    assert(modifier_notifications == 2 && tab_presses == 1 && tab_releases == 1);
    fail_endpoint = false; callback_at(scheduled_at); no_owned_keys();
    assert(!swapper.report_pending && modifier_notifications == 2); cases++;

    printf("%u owned swapper cases passed; no idle cancellation; pinned queue failure negative control observed\n", cases);
    return 0;
}
