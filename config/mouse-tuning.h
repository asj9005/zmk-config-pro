/* Mouse tuning values shared by the keymap and input listeners. */
#ifndef TOTEM_MOUSE_TUNING_H
#define TOTEM_MOUSE_TUNING_H

/* Raise early movement 12.5% from 2400; keep the 900 ms / 4500 peak. */
#define ZMK_POINTING_DEFAULT_MOVE_VAL 2700
#define TOTEM_MOUSE_TIME_TO_MAX_MS 900
#define TOTEM_MOUSE_ACCEL_EXPONENT 2
#define TOTEM_MOUSE_TRIGGER_PERIOD_MS 16
#define TOTEM_MOUSE_TAPPING_TERM_MS 180

/* Add the late curve after 500 ms; reach 2700 * 5/3 = 4500 at 900 ms. */
#define TOTEM_MOUSE_BOOST_START_MS 500
#define TOTEM_MOUSE_BOOST_NUM 5
#define TOTEM_MOUSE_BOOST_DEN 3

/* Absolute cursor speeds, independent of the normal acceleration settings. */
#define TOTEM_MOUSE_FAST_SPEED_NUM 15750
#define TOTEM_MOUSE_FAST_SPEED_DEN 1
#define TOTEM_MOUSE_SLOW_SPEED_NUM 1125
#define TOTEM_MOUSE_SLOW_SPEED_DEN 2

/* AHK wheel presets relative to the observed Windows baseline: 5 lines, 3 chars. */
#define TOTEM_MOUSE_FAST_WHEEL_NUM 4
#define TOTEM_MOUSE_FAST_WHEEL_DEN 1
#define TOTEM_MOUSE_SLOW_WHEEL_NUM 1
#define TOTEM_MOUSE_SLOW_WHEEL_DEN 5
#define TOTEM_MOUSE_FAST_HWHEEL_NUM 5
#define TOTEM_MOUSE_FAST_HWHEEL_DEN 3
#define TOTEM_MOUSE_SLOW_HWHEEL_NUM 1
#define TOTEM_MOUSE_SLOW_HWHEEL_DEN 3

#endif
