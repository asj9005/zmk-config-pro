/* SPDX-License-Identifier: MIT */
int main(void) {
    struct zmk_widget_battery_circles widget = {0};
    current_queue = &system_queue;
    cached_connected[0] = cached_connected[1] = true;
    cached_battery[0] = cached_battery[1] = 0;
    /* Both getters return success+0, but only the left has a real sample. */
    zmk_event_t early = {.kind = BATTERY, .battery = {0, 0}};
    battery_circles_listener(&early);
    assert(!widget_battery_circles_work.pending && lvgl_calls == 0);
    current_queue = &display_queue;
    assert(zmk_widget_battery_circles_init(&widget, NULL) == 0);
    assert(peripheral_connected[0] && peripheral_connected[1]);
    assert(peripheral_battery_known[0] && !peripheral_battery_known[1]);
    assert(strcmp(peripheral_labels[0]->text, "0") == 0);
    assert(strcmp(peripheral_labels[1]->text, "-") == 0);

    display_initialized = true;
    current_queue = &system_queue;
    unsigned int before = lvgl_calls;
    early.battery = (struct zmk_peripheral_battery_state_changed){1, 0};
    battery_circles_listener(&early);
    assert(lvgl_calls == before);
    run_work(&widget_battery_circles_work);
    assert(peripheral_battery_known[1] && lvgl_calls > before);
    assert(strcmp(peripheral_labels[1]->text, "0") == 0);

    /* Positive transport-cache values can seed display init without events. */
    cached_battery[0] = 87; cached_battery[1] = 41;
    current_queue = &display_queue;
    widget_battery_circles_init();
    assert(peripheral_battery[0] == 87 && peripheral_battery[1] == 41);
    assert(strcmp(peripheral_labels[0]->text, "87") == 0);
    assert(strcmp(peripheral_labels[1]->text, "41") == 0);

    current_queue = &system_queue;
    before = lvgl_calls;
    zmk_event_t event = {.kind = BATTERY, .battery = {0, 20}};
    battery_circles_listener(&event);
    event.battery = (struct zmk_peripheral_battery_state_changed){1, 99};
    battery_circles_listener(&event);
    event = (zmk_event_t){.kind = CONNECTION, .connection = {0, false}};
    battery_circles_listener(&event);
    event.connection = (struct zmk_split_central_status_changed){1, true};
    battery_circles_listener(&event);
    assert(lvgl_calls == before);
    run_work(&widget_battery_circles_work);
    assert(peripheral_battery[0] == 20 && peripheral_battery[1] == 99);
    assert(!peripheral_connected[0] && peripheral_connected[1]);
    assert(strcmp(peripheral_labels[0]->text, "-") == 0);
    assert(strcmp(peripheral_labels[1]->text, "99") == 0);

    before = lvgl_calls;
    battery_circles_listener(&event);
    event = (zmk_event_t){.kind = BATTERY, .battery = {255, 88}};
    battery_circles_listener(&event);
    event = (zmk_event_t){.kind = CONNECTION, .connection = {255, true}};
    battery_circles_listener(&event);
    event = (zmk_event_t){.kind = BATTERY, .battery = {1, 255}};
    battery_circles_listener(&event);
    run_work(&widget_battery_circles_work);
    assert(lvgl_calls == before);

    event = (zmk_event_t){.kind = CONNECTION, .connection = {0, true}};
    battery_circles_listener(&event);
    event = (zmk_event_t){.kind = BATTERY, .battery = {1, 0}};
    battery_circles_listener(&event);
    run_work(&widget_battery_circles_work);
    assert(strcmp(peripheral_labels[0]->text, "20") == 0);
    assert(strcmp(peripheral_labels[1]->text, "0") == 0);
    puts("Battery: cached init, early measured zero vs unknown, both-source bursts, duplicate suppression and invalid samples pass");
    return 0;
}
