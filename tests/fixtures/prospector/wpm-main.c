/* SPDX-License-Identifier: MIT */
int main(void) {
    struct zmk_widget_wpm_meter widget = {0};
    current_queue = &display_queue;
    fake_wpm = 100;
    assert(zmk_widget_wpm_meter_init(&widget, NULL) == 0);
    assert(wpm_smooth_work.work.pending);
    assert(wpm_smooth_work.work.queue == &display_queue);
    run_work(&wpm_smooth_work.work);
    assert((int)(displayed_wpm + 0.5f) == 30);
    assert(strcmp(widget.wpm_label->text, "30") == 0);
    assert(peak_position == 6 && wpm_smooth_work.work.delay == 33);
    for (int i = 0; i < 100 && wpm_smooth_work.work.pending; i++) {
        run_work(&wpm_smooth_work.work);
    }
    assert(!wpm_smooth_work.work.pending && displayed_wpm == 100.0f);
    assert(strcmp(widget.wpm_label->text, "100") == 0);

    display_initialized = true;
    current_queue = &system_queue;
    fake_wpm = 0;
    zmk_event_t event = {.kind = OTHER};
    unsigned int before = lvgl_calls;
    widget_wpm_meter_cb(&event);
    assert(lvgl_calls == before);
    run_work(&widget_wpm_meter_work);
    run_work(&wpm_smooth_work.work);
    assert((int)(displayed_wpm + 0.5f) == 95);
    for (int i = 0; i < 1000 && wpm_smooth_work.work.pending; i++) {
        run_work(&wpm_smooth_work.work);
    }
    assert(!wpm_smooth_work.work.pending && displayed_wpm == 0.0f && peak_position == 0);
    assert(strcmp(widget.wpm_label->text, "0") == 0);
    assert(strcmp(widget.layer_label->text, "BASE") == 0);
    puts("WPM: initialized before scheduling, display queue frames, rise/decay/peak/idle and layer text pass");
    return 0;
}
