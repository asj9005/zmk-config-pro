/* SPDX-License-Identifier: MIT */

/* ZMK stores subscriptions in link order, without a numeric priority API.
 * Enforce the resulting addresses as well as the CMake source ordering. */
#if defined(CONFIG_DT_HAS_TOTEM_BEHAVIOR_OWNED_SWAPPER_ENABLED)
#define TOTEM_SWAPPER_RAW zmk_event_sub_totem_owned_swapperzmk_position_state_changed
#define TOTEM_SWAPPER_REPLAY zmk_event_sub_totem_owned_swapper_replayzmk_position_state_changed
#define TOTEM_SWAPPER_KEYMAP zmk_event_sub_keymapzmk_position_state_changed

ASSERT(TOTEM_SWAPPER_RAW < TOTEM_SWAPPER_REPLAY,
       "Owned swapper raw listener must precede replay listener")
ASSERT(TOTEM_SWAPPER_REPLAY < TOTEM_SWAPPER_KEYMAP,
       "Owned swapper replay listener must precede keymap listener")

#if defined(CONFIG_ZMK_BEHAVIOR_HOLD_TAP) && defined(CONFIG_DT_HAS_ZMK_BEHAVIOR_HOLD_TAP_ENABLED)
#define TOTEM_SWAPPER_HOLD_TAP zmk_event_sub_behavior_hold_tapzmk_position_state_changed
ASSERT(TOTEM_SWAPPER_RAW < TOTEM_SWAPPER_HOLD_TAP && TOTEM_SWAPPER_HOLD_TAP < TOTEM_SWAPPER_REPLAY,
       "Owned swapper requires raw < hold-tap < replay")
#endif

#if defined(CONFIG_DT_HAS_ZMK_COMBOS_ENABLED)
#define TOTEM_SWAPPER_COMBO zmk_event_sub_combozmk_position_state_changed
ASSERT(TOTEM_SWAPPER_RAW < TOTEM_SWAPPER_COMBO && TOTEM_SWAPPER_COMBO < TOTEM_SWAPPER_REPLAY,
       "Owned swapper requires raw < combo < replay")
#endif

#if defined(CONFIG_ZMK_BEHAVIOR_TAP_DANCE) && defined(CONFIG_DT_HAS_ZMK_BEHAVIOR_TAP_DANCE_ENABLED)
#define TOTEM_SWAPPER_TAP_DANCE zmk_event_sub_behavior_tap_dancezmk_position_state_changed
ASSERT(TOTEM_SWAPPER_RAW < TOTEM_SWAPPER_TAP_DANCE && TOTEM_SWAPPER_TAP_DANCE < TOTEM_SWAPPER_REPLAY,
       "Owned swapper requires raw < tap-dance < replay")
#endif

#endif
