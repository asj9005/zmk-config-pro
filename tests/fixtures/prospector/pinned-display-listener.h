/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Pinned reference: zmkfirmware/zmk 904c9aec8822d79149d42c8a9a77e8828eb08f5a
 * app/include/zmk/display.h -- macro body copied unchanged.
 */
#define ZMK_DISPLAY_WIDGET_LISTENER(listener, state_type, cb, state_func)                          \
    K_MUTEX_DEFINE(listener##_mutex);                                                              \
    static state_type __##listener##_state;                                                        \
    static state_type listener##_get_local_state() {                                               \
        k_mutex_lock(&listener##_mutex, K_FOREVER);                                                \
        state_type copy = __##listener##_state;                                                    \
        k_mutex_unlock(&listener##_mutex);                                                         \
        return copy;                                                                               \
    };                                                                                             \
    static void listener##_work_cb(struct k_work *work) { cb(listener##_get_local_state()); };     \
    K_WORK_DEFINE(listener##_work, listener##_work_cb);                                            \
    static void listener##_refresh_state(const zmk_event_t *eh) {                                  \
        k_mutex_lock(&listener##_mutex, K_FOREVER);                                                \
        __##listener##_state = state_func(eh);                                                     \
        k_mutex_unlock(&listener##_mutex);                                                         \
    };                                                                                             \
    static void listener##_init() {                                                                \
        listener##_refresh_state(NULL);                                                            \
        listener##_work_cb(NULL);                                                                  \
    }                                                                                              \
    static int listener##_cb(const zmk_event_t *eh) {                                              \
        if (zmk_display_is_initialized()) {                                                        \
            listener##_refresh_state(eh);                                                          \
            k_work_submit_to_queue(zmk_display_work_q(), &listener##_work);                        \
        }                                                                                          \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(listener, listener##_cb);

