/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#include "output.h"

#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#include <fonts.h>
#include "display_colors.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void set_btn_state(lv_obj_t *btn, bool active, uint32_t active_color,
                          uint32_t inactive_color) {
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (active) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(active_color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(DISPLAY_COLOR_OUTPUT_ACTIVE_TEXT),
                                        LV_PART_MAIN);
        }
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(inactive_color), LV_PART_MAIN);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(inactive_color), LV_PART_MAIN);
        }
    }
}

static void update_output_widget(struct zmk_widget_output *widget,
                                 enum zmk_transport active_transport) {
    bool is_usb = active_transport == ZMK_TRANSPORT_USB;
    set_btn_state(widget->usb_btn, is_usb, DISPLAY_COLOR_USB_ACTIVE_BG,
                  DISPLAY_COLOR_USB_INACTIVE_BG);
    /* ESB is the only split transport in this profile; connection state is
     * shown independently by the two battery circles. */
    set_btn_state(widget->esb_btn, true, DISPLAY_COLOR_BLE_ACTIVE_BG,
                  DISPLAY_COLOR_BLE_INACTIVE_BG);
}

static void output_update_cb(enum zmk_transport active_transport) {
    struct zmk_widget_output *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        update_output_widget(widget, active_transport);
    }
}

static enum zmk_transport output_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return zmk_endpoint_get_selected().transport;
}

/* Endpoint events may run on the system queue. Confine LVGL changes to the
 * display queue, coalescing transitions to the final selected transport. */
ZMK_DISPLAY_WIDGET_LISTENER(widget_output_endpoint, enum zmk_transport,
                           output_update_cb, output_get_state)
ZMK_SUBSCRIPTION(widget_output_endpoint, zmk_endpoint_changed);

static lv_obj_t *create_toggle_btn(lv_obj_t *parent, const char *text, int x) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 56, 29);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_set_style_translate_y(label, 1, LV_PART_MAIN);
    return btn;
}

static lv_obj_t *create_slot(lv_obj_t *parent, const char *text, int x) {
    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_set_size(slot, 57, 29);
    lv_obj_set_pos(slot, x, 33);
    lv_obj_set_style_radius(slot, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot, lv_color_hex(DISPLAY_COLOR_SLOT_INACTIVE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slot, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(slot);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_set_style_translate_y(label, 1, LV_PART_MAIN);
    return slot;
}

int zmk_widget_output_init(struct zmk_widget_output *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 116, 62);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    widget->usb_btn = create_toggle_btn(widget->obj, "USB", 0);
    widget->esb_btn = create_toggle_btn(widget->obj, "ESB", 58);
    widget->slots[0] = create_slot(widget->obj, "L", 0);
    widget->slots[1] = create_slot(widget->obj, "R", 59);

    sys_slist_append(&widgets, &widget->node);
    widget_output_endpoint_init();
    return 0;
}

lv_obj_t *zmk_widget_output_obj(struct zmk_widget_output *widget) { return widget->obj; }
