/* Mouse tuning values shared by the keymap and input listeners. */
#ifndef TOTEM_MOUSE_TUNING_H
#define TOTEM_MOUSE_TUNING_H

/* Keep the old 600 / 300^2 initial acceleration, then continue to 1600. */
#define ZMK_POINTING_DEFAULT_MOVE_VAL 1600
#define TOTEM_MOUSE_TIME_TO_MAX_MS 490
#define TOTEM_MOUSE_ACCEL_EXPONENT 2
#define TOTEM_MOUSE_TRIGGER_PERIOD_MS 16
#define TOTEM_MOUSE_TAPPING_TERM_MS 180

/* Initial approximation of AHK speed 20 / 3 at Windows speed 10, EPP off. */
#define TOTEM_MOUSE_FAST_NUM 7
#define TOTEM_MOUSE_FAST_DEN 2
#define TOTEM_MOUSE_SLOW_NUM 1
#define TOTEM_MOUSE_SLOW_DEN 8

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
