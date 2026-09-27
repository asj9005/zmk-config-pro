/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Return a failure without the Windows CRT abort dialog in negative controls. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "assertion failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); \
} } while (0)

#define ARG_UNUSED(x) ((void)(x))
#define IS_ENABLED(x) (x)
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING 1
#define CONFIG_PROSPECTOR_LAYER_NAME_UPPERCASE 1
#define PERIPHERAL_COUNT 2
#define WPM_BAR_COUNT 26
#define WPM_MAX 120
#define K_MSEC(x) (x)
#define K_NO_WAIT 0
#define K_FOREVER -1
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LISTENER(name, cb)
#define ZMK_SUBSCRIPTION(listener, event)

struct k_work_q { int identity; };
static struct k_work_q display_queue, system_queue;
static struct k_work_q *current_queue;
static bool display_initialized;
static struct k_work_q *zmk_display_work_q(void) { return &display_queue; }
static bool zmk_display_is_initialized(void) { return display_initialized; }
struct k_work {
    void (*handler)(struct k_work *);
    struct k_work_q *queue;
    bool pending;
    int delay;
};
struct k_work_delayable { struct k_work work; };
#define K_WORK_DEFINE(name, cb) static struct k_work name = {.handler = cb}
#define K_MUTEX_DEFINE(name) static int name
static void k_mutex_lock(int *mutex, int timeout) { assert((*mutex)++ == 0); }
static void k_mutex_unlock(int *mutex) { assert(--*mutex == 0); }
static void k_work_init_delayable(struct k_work_delayable *work, void (*handler)(struct k_work *)) {
    assert(!work->work.pending);
    work->work.handler = handler;
}
static int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    assert(queue == &display_queue && work->handler != NULL);
    work->queue = queue;
    work->pending = true;
    return 0;
}
static int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work, int delay) {
    assert(queue == &display_queue && work->work.handler != NULL);
    if (!work->work.pending) {
        work->work.queue = queue;
        work->work.pending = true;
        work->work.delay = delay;
    }
    return 0;
}
static int k_work_schedule(struct k_work_delayable *work, int delay) {
    return k_work_schedule_for_queue(&system_queue, work, delay);
}
static void run_work(struct k_work *work) {
    assert(work->pending && work->queue == &display_queue);
    struct k_work_q *previous = current_queue;
    current_queue = work->queue;
    work->pending = false;
    work->handler(work);
    current_queue = previous;
}

typedef struct sys_snode { struct sys_snode *next; } sys_snode_t;
typedef struct { sys_snode_t *head; } sys_slist_t;
#define SYS_SLIST_STATIC_INIT(p) {NULL}
#define SYS_SLIST_FOR_EACH_CONTAINER(list, item, member) \
    for ((item) = (void *)(list)->head; (item); (item) = (void *)(item)->member.next)
static void sys_slist_append(sys_slist_t *list, sys_snode_t *node) {
    node->next = NULL;
    sys_snode_t **tail = &list->head;
    while (*tail) { tail = &(*tail)->next; }
    *tail = node;
}

struct zmk_peripheral_battery_state_changed { uint8_t source, state_of_charge; };
struct zmk_split_central_status_changed { uint8_t slot; bool connected; };
typedef struct {
    enum { BATTERY, CONNECTION, ENDPOINT, OTHER } kind;
    struct zmk_peripheral_battery_state_changed battery;
    struct zmk_split_central_status_changed connection;
} zmk_event_t;
static const struct zmk_peripheral_battery_state_changed *as_zmk_peripheral_battery_state_changed(const zmk_event_t *event) {
    return event->kind == BATTERY ? &event->battery : NULL;
}
static const struct zmk_split_central_status_changed *as_zmk_split_central_status_changed(const zmk_event_t *event) {
    return event->kind == CONNECTION ? &event->connection : NULL;
}
static bool cached_connected[PERIPHERAL_COUNT];
static uint8_t cached_battery[PERIPHERAL_COUNT];
static int cached_battery_error[PERIPHERAL_COUNT];
static bool totem_esb_peer_is_connected(uint8_t source) { return cached_connected[source]; }
static int zmk_split_central_get_peripheral_battery_level(uint8_t source, uint8_t *level) {
    *level = cached_battery[source];
    return cached_battery_error[source];
}
enum zmk_transport { ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance { enum zmk_transport transport; };
static struct zmk_endpoint_instance selected_endpoint;
static struct zmk_endpoint_instance zmk_endpoint_get_selected(void) { return selected_endpoint; }
static uint8_t fake_wpm, fake_layer;
static uint8_t zmk_wpm_get_state(void) { return fake_wpm; }
static uint8_t zmk_keymap_highest_layer_active(void) { return fake_layer; }
static uint8_t zmk_keymap_layer_index_to_id(uint8_t index) { return index; }
static const char *zmk_keymap_layer_name(uint8_t id) { return "Base"; }

typedef uint32_t lv_color_t;
typedef struct { char text[32]; int value; } lv_obj_t;
typedef struct { int unused; } lv_style_t;
typedef struct { int32_t act_time, duration, end_value, start_value; } lv_anim_t;
static unsigned int lvgl_calls;
static lv_obj_t objects[256];
static unsigned int object_count;
static void fake_lvgl(void) { assert(current_queue == &display_queue); lvgl_calls++; }
static lv_obj_t *new_object(lv_obj_t *parent) {
    fake_lvgl(); assert(object_count < 256); return &objects[object_count++];
}
static void lv_label_set_text(lv_obj_t *object, const char *text) {
    fake_lvgl(); snprintf(object->text, sizeof(object->text), "%s", text);
}
static void lv_arc_set_value(lv_obj_t *object, int value) { fake_lvgl(); object->value = value; }
static int lv_arc_get_value(lv_obj_t *object) { fake_lvgl(); return object->value; }
static int lv_obj_get_style_arc_width(lv_obj_t *object, int part) { fake_lvgl(); return 2; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *object, int index) { fake_lvgl(); return object; }
static int lv_anim_path_ease_out(const lv_anim_t *animation) { return 0; }
#define lv_color_hex(x) (x)
#define lv_obj_create(parent) new_object(parent)
#define lv_arc_create(parent) new_object(parent)
#define lv_bar_create(parent) new_object(parent)
#define lv_label_create(parent) new_object(parent)
#define lv_style_init(...) fake_lvgl()
#define lv_style_set_arc_color(...) fake_lvgl()
#define lv_style_set_bg_color(...) fake_lvgl()
#define lv_style_set_text_color(...) fake_lvgl()
#define lv_obj_set_style_arc_width(...) fake_lvgl()
#define lv_anim_init(...) fake_lvgl()
#define lv_anim_set_var(...) fake_lvgl()
#define lv_anim_set_values(...) fake_lvgl()
#define lv_anim_set_time(...) fake_lvgl()
#define lv_anim_set_exec_cb(...) fake_lvgl()
#define lv_anim_set_path_cb(...) fake_lvgl()
#define lv_anim_start(...) fake_lvgl()
#define lv_obj_remove_style(...) fake_lvgl()
#define lv_obj_add_style(...) fake_lvgl()
#define lv_obj_set_style_bg_color(...) fake_lvgl()
#define lv_bar_set_value(...) fake_lvgl()
#define lv_obj_align_to(...) fake_lvgl()
#define lv_obj_set_size(...) fake_lvgl()
#define lv_obj_set_style_bg_opa(...) fake_lvgl()
#define lv_obj_set_style_border_width(...) fake_lvgl()
#define lv_obj_set_style_pad_all(...) fake_lvgl()
#define lv_obj_align(...) fake_lvgl()
#define lv_arc_set_range(...) fake_lvgl()
#define lv_arc_set_bg_angles(...) fake_lvgl()
#define lv_arc_set_rotation(...) fake_lvgl()
#define lv_obj_clear_flag(...) fake_lvgl()
#define lv_obj_set_pos(...) fake_lvgl()
#define lv_obj_set_style_radius(...) fake_lvgl()
#define lv_obj_set_style_text_font(...) fake_lvgl()
#define lv_obj_set_style_text_letter_space(...) fake_lvgl()
#define lv_obj_set_style_text_align(...) fake_lvgl()
#define lv_bar_set_range(...) fake_lvgl()
#define lv_obj_add_flag(...) fake_lvgl()
#define lv_obj_set_style_text_color(...) fake_lvgl()
#define lv_obj_set_style_pad_hor(...) fake_lvgl()
#define lv_obj_set_style_pad_ver(...) fake_lvgl()
#define lv_obj_set_style_pad_top(...) fake_lvgl()
#define lv_obj_set_style_pad_bottom(...) fake_lvgl()
#define lv_obj_set_style_border_color(...) fake_lvgl()
#define lv_obj_center(...) fake_lvgl()
#define lv_obj_set_style_translate_y(...) fake_lvgl()

struct zmk_widget_battery_circles { sys_snode_t node; lv_obj_t *obj; bool initialized; };
struct zmk_widget_wpm_meter {
    sys_snode_t node; lv_obj_t *obj; lv_obj_t *bars[WPM_BAR_COUNT];
    lv_obj_t *peak_indicator, *wpm_label, *layer_label;
};
struct zmk_widget_output {
    sys_snode_t node; lv_obj_t *obj; lv_obj_t *usb_btn, *esb_btn; lv_obj_t *slots[2];
};
