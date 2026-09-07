/* SPDX-License-Identifier: MIT */
#ifndef TOTEM_POINTING_CURVE_H
#define TOTEM_POINTING_CURVE_H

#include <stdint.h>

#if !defined(CONFIG_MINIMAL_LIBC) || !CONFIG_MINIMAL_LIBC
#include <math.h>
#endif

static inline float totem_pointing_curve_power(float base, uint8_t exponent) {
#if defined(CONFIG_MINIMAL_LIBC) && CONFIG_MINIMAL_LIBC
    /* Preserve the pinned two-axis behavior's minimal-libc calculation. */
    float power = 1.0f;
    for (uint8_t i = 0; i < exponent; i++) {
        power = power * base;
    }
    return power;
#else
    return powf(base, (float)exponent);
#endif
}

/* max_speed retains the binding's direction. The optional extra curve starts
 * at zero after boost_start_ms and reaches (numerator / denominator - 1) times
 * max_speed at time_to_max_ms. Disabled/invalid boost preserves the old curve.
 * Uniform movement (zero duration or exponent) never applies the boost.
 */
static inline float totem_pointing_curve_speed(float max_speed, uint32_t elapsed_ms,
                                               uint16_t time_to_max_ms, uint8_t exponent,
                                               uint16_t boost_start_ms,
                                               uint16_t boost_numerator,
                                               uint16_t boost_denominator) {
    if (time_to_max_ms == 0 || exponent == 0) {
        return max_speed;
    }

    const int boosted = boost_denominator != 0 && boost_numerator > boost_denominator &&
                        boost_start_ms < time_to_max_ms;
    const float multiplier = boosted ? (float)boost_numerator / boost_denominator : 1.0f;

    if (elapsed_ms > time_to_max_ms) {
        return boosted ? max_speed * multiplier : max_speed;
    }
    if (elapsed_ms == 0) {
        return 0;
    }

    /* Keep the original floating-point operation order before adding a boost. */
    const float time_fraction = (float)elapsed_ms / time_to_max_ms;
    const float base = max_speed * totem_pointing_curve_power(time_fraction, exponent);
    if (!boosted || elapsed_ms <= boost_start_ms) {
        return base;
    }
    if (elapsed_ms == time_to_max_ms) {
        return max_speed * multiplier;
    }

    const float boost_fraction =
        (float)(elapsed_ms - boost_start_ms) / (time_to_max_ms - boost_start_ms);
    return base + max_speed * (multiplier - 1.0f) *
                      totem_pointing_curve_power(boost_fraction, exponent);
}

#endif /* TOTEM_POINTING_CURVE_H */
