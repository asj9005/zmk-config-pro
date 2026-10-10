/* SPDX-License-Identifier: MIT */
static void check_colors(struct zmk_widget_modifier_indicator *widget) {
    struct modifier_indicator_state state = modifier_indicator_get_state(NULL);
    for (int i = 0; i < 4; i++) {
        enum modifier_type type = modifier_order_get(i);
        lv_color_t expected = state.mods[type] ? DISPLAY_COLOR_MOD_ACTIVE : DISPLAY_COLOR_MOD_INACTIVE;
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
        if (type == MOD_TYPE_SHIFT && state.caps_word) { expected = DISPLAY_COLOR_MOD_CAPS_WORD; }
#endif
        assert(widget->mod_labels[i]->text_color == expected);
    }
}

int main(void) {
    struct zmk_widget_modifier_indicator first = {0}, second = {0};
    current_queue = &display_queue;
    fake_mods = MOD_LALT;
    assert(zmk_widget_modifier_indicator_init(&first, NULL) == 0);
    check_colors(&first);
    display_initialized = true;
    current_queue = &system_queue;
    unsigned int before = color_writes;
    zmk_event_t event = {.kind = OTHER};
    /* Ordinary key presses/releases must not redraw unchanged modifiers. */
    for (int i = 0; i < 100; i++) {
        widget_modifier_indicator_cb(&event);
        run_work(&widget_modifier_indicator_work);
    }
    assert(color_writes == before);
    fake_mods = MOD_RALT; /* Left/right Alt have the same displayed state. */
    widget_modifier_indicator_cb(&event);
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before);

    fake_mods = MOD_RCTL | MOD_RSFT;
    widget_modifier_indicator_cb(&event);
    fake_mods = MOD_RGUI;
    widget_modifier_indicator_cb(&event);
    assert(color_writes == before); /* No LVGL calls on the input thread. */
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before + 4);
    check_colors(&first);

    /* A second widget must get current state even when globally unchanged. */
    current_queue = &display_queue;
    assert(zmk_widget_modifier_indicator_init(&second, NULL) == 0);
    check_colors(&first); check_colors(&second);
    current_queue = &system_queue;
    before = color_writes;
    widget_modifier_indicator_cb(&event);
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before);

#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
    zmk_event_t caps_event = {.kind = CAPS_WORD, .caps_word.active = true};
    widget_modifier_indicator_cb(&caps_event);
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before + 8);
    check_colors(&first); check_colors(&second);
    before = color_writes;
    widget_modifier_indicator_cb(&event);
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before);
    caps_event.caps_word.active = false;
    widget_modifier_indicator_cb(&caps_event);
    run_work(&widget_modifier_indicator_work);
    assert(color_writes == before + 8);
#endif
    fake_mods = 0;
    widget_modifier_indicator_cb(&event);
    run_work(&widget_modifier_indicator_work);
    check_colors(&first); check_colors(&second);
    puts("Modifiers: duplicate events skip redraw; changes, Caps Word and new widgets render correctly");
    return 0;
}
