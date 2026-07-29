/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#define TOTEM_ESB_OUTPUT_SLOTS 2

struct zmk_widget_output {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *usb_btn;
    lv_obj_t *esb_btn;
    lv_obj_t *slots[TOTEM_ESB_OUTPUT_SLOTS];
};

int zmk_widget_output_init(struct zmk_widget_output *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_output_obj(struct zmk_widget_output *widget);
