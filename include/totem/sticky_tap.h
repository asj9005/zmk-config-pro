/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct device;

/* Consume a released one-shot from this sticky behavior and emit its normal
 * key-up. Physical keys and other sticky behaviors retain their own presses. */
bool totem_sticky_consume_tap(const struct device *sticky, uint32_t keycode, int64_t timestamp);
