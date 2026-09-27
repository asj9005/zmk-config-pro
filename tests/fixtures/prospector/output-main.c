/* SPDX-License-Identifier: MIT */
int main(void) {
    struct zmk_widget_output widget = {0};
    current_queue = &display_queue;
    selected_endpoint.transport = ZMK_TRANSPORT_USB;
    assert(zmk_widget_output_init(&widget, NULL) == 0);
    display_initialized = true;
    current_queue = &system_queue;
    unsigned int before = lvgl_calls;
    zmk_event_t event = {.kind = ENDPOINT};
    selected_endpoint.transport = ZMK_TRANSPORT_BLE;
    widget_output_endpoint_cb(&event);
    selected_endpoint.transport = ZMK_TRANSPORT_USB;
    widget_output_endpoint_cb(&event);
    assert(lvgl_calls == before);
    assert(__widget_output_endpoint_state == ZMK_TRANSPORT_USB);
    run_work(&widget_output_endpoint_work);
    assert(lvgl_calls > before);
    puts("Output: endpoint bursts retain latest state and LVGL runs only on display queue");
    return 0;
}
