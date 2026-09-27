/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2020 The ZMK Contributors
 * Exact modifier reference-count function extracts from zmkfirmware/zmk,
 * commit 904c9aec8822d79149d42c8a9a77e8828eb08f5a, app/src/hid.c.
 * This file supplies test references, not independently compiled firmware.
 */

int zmk_hid_register_mod(zmk_mod_t modifier) {
    explicit_modifier_counts[modifier]++;
    LOG_DBG("Modifier %d count %d", modifier, explicit_modifier_counts[modifier]);
    WRITE_BIT(explicit_modifiers, modifier, true);
    zmk_mod_flags_t current = GET_MODIFIERS;
    SET_MODIFIERS(explicit_modifiers);
    return current == GET_MODIFIERS ? 0 : 1;
}

int zmk_hid_unregister_mod(zmk_mod_t modifier) {
    if (explicit_modifier_counts[modifier] <= 0) {
        LOG_ERR("Tried to unregister modifier %d too often", modifier);
        return -EINVAL;
    }
    explicit_modifier_counts[modifier]--;
    LOG_DBG("Modifier %d count: %d", modifier, explicit_modifier_counts[modifier]);
    if (explicit_modifier_counts[modifier] == 0) {
        LOG_DBG("Modifier %d released", modifier);
        WRITE_BIT(explicit_modifiers, modifier, false);
    }
    zmk_mod_flags_t current = GET_MODIFIERS;
    SET_MODIFIERS(explicit_modifiers);
    return current == GET_MODIFIERS ? 0 : 1;
}

int zmk_hid_register_mods(zmk_mod_flags_t modifiers) {
    int ret = 0;
    for (zmk_mod_t i = 0; i < 8; i++) {
        if (modifiers & (1 << i)) {
            ret += zmk_hid_register_mod(i);
        }
    }
    return ret;
}

int zmk_hid_unregister_mods(zmk_mod_flags_t modifiers) {
    int ret = 0;
    for (zmk_mod_t i = 0; i < 8; i++) {
        if (modifiers & (1 << i)) {
            ret += zmk_hid_unregister_mod(i);
        }
    }

    return ret;
}

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 The ZMK Contributors
 * Exact legacy negative-control extract from urob/zmk-tri-state,
 * commit 2007896c6d5bfb519e8babccf8633841c5647d8b,
 * behaviors/behavior_tri_state.c.
 */

void trigger_end_behavior(struct active_tri_state *si) {
    struct zmk_behavior_binding_event event = {
        .position = si->position,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = si->source,
#endif
    };

    zmk_behavior_queue_add(&event, si->config->end_behavior, true, si->config->tap_ms);
    zmk_behavior_queue_add(&event, si->config->end_behavior, false, 0);
}
