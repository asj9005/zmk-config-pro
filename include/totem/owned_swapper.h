/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Invoke after synthetic physical releases, including session replacement. */
void totem_owned_swapper_source_reset(uint8_t source);

struct zmk_position_state_changed;
/* Late bridge: end on nonignored replayed input without rewriting raw state. */
int totem_owned_swapper_replayed_position(const struct zmk_position_state_changed *event);
