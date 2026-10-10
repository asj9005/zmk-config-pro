/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TOTEM_FIELD_BINS 18U
/* Inclusive upper bounds in microseconds; bin 17 has no upper bound. */
static const uint32_t totem_field_bin_upper_us[TOTEM_FIELD_BINS - 1U] = {
    100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000,
    100000, 150000, 180000, 200000, 220000, 250000, 500000, 1000000,
};

struct totem_field_stats {
    uint64_t sum_us;
    uint32_t count;
    uint32_t max_us;
    uint32_t bins[TOTEM_FIELD_BINS];
    /* bit 0: count limit reached, bit 1: sum saturated, bit 2: max clipped. */
    uint32_t flags;
};

static inline uint32_t totem_field_sat_inc(uint32_t value) {
    return value == UINT32_MAX ? value : value + 1U;
}

static inline void totem_field_stats_reset(struct totem_field_stats *stats) {
    memset(stats, 0, sizeof(*stats));
}

/* Stop admitting samples at the count limit so the histogram still sums to
 * count. Cumulative metrics never wrap into plausible small measurements. */
static inline bool totem_field_stats_observe(struct totem_field_stats *stats,
                                            uint64_t duration_us) {
    if (stats->count == UINT32_MAX) {
        stats->flags |= 1U;
        return false;
    }
    unsigned int bin = 0;
    while (bin < TOTEM_FIELD_BINS - 1U && duration_us > totem_field_bin_upper_us[bin]) {
        bin++;
    }
    stats->count++;
    stats->bins[bin]++;
    if (UINT64_MAX - stats->sum_us < duration_us) {
        stats->sum_us = UINT64_MAX;
        stats->flags |= 2U;
    } else {
        stats->sum_us += duration_us;
    }
    uint32_t bounded = duration_us > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_us;
    if (duration_us > UINT32_MAX) {
        stats->flags |= 4U;
    }
    if (bounded > stats->max_us) {
        stats->max_us = bounded;
    }
    return true;
}

/* Progress may take several short FIFO writes, but never extends the deadline.
 * The next frame starts with a newline and a new seq after an aborted prefix. */
struct totem_field_output_state {
    uint64_t deadline_ms;
    uint32_t sequence;
    uint32_t dropped;
    bool active;
};

static inline void totem_field_output_begin(struct totem_field_output_state *state,
                                           uint64_t now_ms, uint32_t budget_ms) {
    state->sequence++;
    if (state->sequence == 0) {
        state->sequence = 1;
    }
    state->active = true;
    state->deadline_ms = now_ms + budget_ms;
}

static inline void totem_field_output_abort(struct totem_field_output_state *state) {
    if (state->active) {
        state->dropped = totem_field_sat_inc(state->dropped);
        state->active = false;
    }
}

static inline bool totem_field_output_accept(struct totem_field_output_state *state,
                                            unsigned int requested, int accepted) {
    if (!state->active || accepted < 0 || (unsigned int)accepted > requested) {
        totem_field_output_abort(state);
        return false;
    }
    return true;
}

static inline bool totem_field_output_ready(struct totem_field_output_state *state,
                                           uint64_t now_ms, bool connected) {
    if (!state->active || !connected || now_ms >= state->deadline_ms) {
        totem_field_output_abort(state);
        return false;
    }
    return true;
}
