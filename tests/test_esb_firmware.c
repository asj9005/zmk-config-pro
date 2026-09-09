/* Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 * Host regressions for the actual portable firmware helpers.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <totem/esb_backoff.h>
#include <totem/esb_key_state.h>

/* Keep the queue small so ordinary scenarios exercise saturation and reuse. */
#define CONFIG_ESB_PIPE_COUNT 3
#ifndef CONFIG_ESB_MAX_PAYLOAD_LENGTH
#define CONFIG_ESB_MAX_PAYLOAD_LENGTH 48
#endif
#ifndef CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS
#define CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS 6
#endif
#include <totem/esb_prx_queue.h>

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            exit(EXIT_FAILURE);                                                  \
        }                                                                        \
    } while (0)

struct transition {
    uint8_t position;
    bool pressed;
};

struct observed_events {
    uint8_t *state;
    struct transition events[256];
    bool seen[256];
    size_t count;
    bool saw_press;
};

static bool fixture_bit(const uint8_t *bitmap, size_t position) {
    return (bitmap[position / 8U] & (uint8_t)(1U << (position % 8U))) != 0;
}

static void fixture_press(uint8_t *bitmap, size_t position) {
    bitmap[position / 8U] |= (uint8_t)(1U << (position % 8U));
}

static void record_event(void *context, uint8_t position, bool pressed) {
    struct observed_events *observed = context;
    CHECK(observed->count < 256U);
    CHECK(!observed->seen[position]);
    CHECK(fixture_bit(observed->state, position) == pressed);
    /* A newly pressed modifier/key cannot precede any outstanding release. */
    CHECK(pressed || !observed->saw_press);
    observed->saw_press |= pressed;
    observed->seen[position] = true;
    observed->events[observed->count++] = (struct transition){position, pressed};
}

static void missing_release_clears_stuck_key(void) {
    uint8_t current[5] = {0};
    uint8_t desired[5] = {0};
    fixture_press(current, 12U);
    struct observed_events observed = {.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 1U);
    CHECK(observed.count == 1U);
    CHECK(observed.events[0].position == 12U);
    CHECK(!observed.events[0].pressed);
    CHECK(memcmp(current, desired, sizeof(current)) == 0);

    /* A repeated heartbeat must not emit the release twice. */
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 0U);
    CHECK(observed.count == 1U);
}

static void missing_press_is_recovered(void) {
    uint8_t current[5] = {0};
    uint8_t desired[5] = {0};
    fixture_press(desired, 37U);
    struct observed_events observed = {.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 1U);
    CHECK(observed.count == 1U);
    CHECK(observed.events[0].position == 37U);
    CHECK(observed.events[0].pressed);
    CHECK(memcmp(current, desired, sizeof(current)) == 0);
}

static void releases_precede_new_presses(void) {
    uint8_t current[5] = {0};
    uint8_t desired[5] = {0};
    /* The release has a higher position, defeating a single ascending pass. */
    fixture_press(current, 30U);
    fixture_press(desired, 2U);
    struct observed_events observed = {.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 2U);
    CHECK(observed.count == 2U);
    CHECK(observed.events[0].position == 30U && !observed.events[0].pressed);
    CHECK(observed.events[1].position == 2U && observed.events[1].pressed);
}

static void held_keys_return_after_receiver_reset(void) {
    uint8_t current[5] = {0};
    uint8_t desired[5] = {0};
    fixture_press(desired, 0U);
    fixture_press(desired, 19U);
    fixture_press(desired, 37U);
    struct observed_events observed = {.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 3U);
    CHECK(observed.count == 3U);
    CHECK(memcmp(current, desired, sizeof(current)) == 0);
    observed = (struct observed_events){.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 0U);
    CHECK(observed.count == 0U);
}

static void peer_states_are_independent(void) {
    uint8_t peers[2][5] = {{0}};
    uint8_t desired[5] = {0};
    fixture_press(peers[0], 2U);
    fixture_press(peers[1], 7U);
    fixture_press(desired, 3U);
    uint8_t right_before[5];
    memcpy(right_before, peers[1], sizeof(right_before));
    struct observed_events observed = {.state = peers[0]};
    CHECK(totem_esb_key_state_reconcile(peers[0], desired, 38U, record_event,
                                       &observed) == 2U);
    CHECK(memcmp(peers[1], right_before, sizeof(right_before)) == 0);
    CHECK(memcmp(peers[0], desired, sizeof(desired)) == 0);
}

static void bitmap_boundaries_and_padding(void) {
    struct {
        uint8_t before;
        uint8_t bitmap[5];
        uint8_t after;
    } state = {0xA5U, {0, 0, 0, 0, 0xC0U}, 0x5AU};
    uint8_t desired[5] = {0};
    const size_t positions[] = {0U, 7U, 8U, 31U, 37U};
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        fixture_press(desired, positions[i]);
    }
    struct observed_events observed = {.state = state.bitmap};
    CHECK(totem_esb_key_state_reconcile(state.bitmap, desired, 38U, record_event,
                                       &observed) == 5U);
    CHECK(observed.count == 5U);
    CHECK((state.bitmap[4] & 0xC0U) == 0xC0U);
    CHECK(state.before == 0xA5U && state.after == 0x5AU);
    for (size_t i = 0; i < 38U; ++i) {
        CHECK(fixture_bit(state.bitmap, i) == fixture_bit(desired, i));
    }
}

static void zero_and_full_position_range(void) {
    uint8_t current[32] = {0};
    uint8_t desired[32] = {0};
    fixture_press(desired, 255U);
    struct observed_events observed = {.state = current};
    CHECK(totem_esb_key_state_reconcile(current, desired, 0U, record_event,
                                       &observed) == 0U);
    CHECK(observed.count == 0U && current[31] == 0U);
    CHECK(totem_esb_key_state_reconcile(current, desired, 256U, record_event,
                                       &observed) == 1U);
    CHECK(observed.count == 1U);
    CHECK(observed.events[0].position == 255U);
    CHECK(observed.events[0].pressed);
}

static void malformed_snapshot_is_rejected_before_state_changes(void) {
    uint8_t current[5] = {0};
    uint8_t desired[5] = {0};
    fixture_press(current, 12U);
    fixture_press(desired, 2U);
    fixture_press(desired, 38U); /* Outside the 38-position matrix. */
    uint8_t before[5];
    memcpy(before, current, sizeof(before));
    struct observed_events observed = {.state = current};
    CHECK(!totem_esb_key_state_valid(desired, 38U));
    CHECK(totem_esb_key_state_reconcile(current, desired, 38U, record_event,
                                       &observed) == 0U);
    CHECK(observed.count == 0U);
    CHECK(memcmp(before, current, sizeof(current)) == 0);
    CHECK(!totem_esb_key_state_valid(NULL, 38U));
    CHECK(!totem_esb_key_state_valid(desired, 0U));
    CHECK(!totem_esb_key_state_valid(desired, 257U));
}

static void exhaustive_one_byte_transitions(void) {
    for (unsigned int before = 0; before <= UINT8_MAX; ++before) {
        for (unsigned int after = 0; after <= UINT8_MAX; ++after) {
            uint8_t current = (uint8_t)before;
            const uint8_t desired = (uint8_t)after;
            struct observed_events observed = {.state = &current};
            size_t count = totem_esb_key_state_reconcile(
                &current, &desired, 8U, record_event, &observed);
            CHECK(current == desired);
            CHECK(count == observed.count);
            for (size_t position = 0; position < 8U; ++position) {
                bool changed = ((before ^ after) & (1U << position)) != 0;
                CHECK(observed.seen[position] == changed);
            }
        }
    }
}

static void handshake_backoff_grows_caps_and_resets(void) {
    struct totem_esb_backoff backoff;
    const uint32_t expected[] = {2U, 4U, 8U, 16U, 32U, 64U,
                                 128U, 256U, 512U, 1000U, 1000U};
    totem_esb_backoff_reset(&backoff, 2U);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        CHECK(totem_esb_backoff_take(&backoff, 1000U) == expected[i]);
    }
    /* An accepted handshake must restore the fast next reconnection attempt. */
    totem_esb_backoff_reset(&backoff, 2U);
    CHECK(totem_esb_backoff_take(&backoff, 1000U) == 2U);
    CHECK(totem_esb_backoff_take(&backoff, 1000U) == 4U);
}

static void handshake_backoff_saturates_without_integer_wrap(void) {
    struct totem_esb_backoff backoff;
    totem_esb_backoff_reset(&backoff, UINT32_MAX / 2U + 1U);
    CHECK(totem_esb_backoff_take(&backoff, UINT32_MAX) == UINT32_MAX / 2U + 1U);
    CHECK(totem_esb_backoff_take(&backoff, UINT32_MAX) == UINT32_MAX);
    CHECK(totem_esb_backoff_take(&backoff, UINT32_MAX) == UINT32_MAX);
    totem_esb_backoff_reset(&backoff, 2048U);
    CHECK(totem_esb_backoff_take(&backoff, 1000U) == 1000U);
    CHECK(totem_esb_backoff_take(&backoff, 1000U) == 1000U);
}

static struct totem_esb_prx_packet fixture_packet(uint8_t pipe, uint8_t value,
                                                  uint32_t timestamp) {
    struct totem_esb_prx_packet packet = {
        .msg_id = value, .length = 2U, .pipe = pipe, .enqueued_at = timestamp,
    };
    packet.data[0] = value;
    packet.data[1] = (uint8_t)~value;
    return packet;
}

static void offline_peer_cannot_starve_active_peer(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    for (uint8_t i = 1U; i <= TOTEM_ESB_PRX_PIPE_LIMIT; ++i) {
        struct totem_esb_prx_packet queued = fixture_packet(1U, i, 100U + i);
        CHECK(totem_esb_prx_offer(&queue, &queued) == 0);
    }
    struct totem_esb_prx_packet overflow = fixture_packet(1U, 253U, 200U);
    struct totem_esb_prx_packet online = fixture_packet(2U, 254U, 201U);
    CHECK(totem_esb_prx_offer(&queue, &overflow) == -ENOSPC);
    CHECK(totem_esb_prx_offer(&queue, &online) == 0);
    CHECK(totem_esb_prx_next(&queue, 1U << 1U) == 2);
    CHECK(totem_esb_prx_peek(&queue, 2U)->data[0] == 254U);
    totem_esb_prx_pop(&queue, 2U);
    CHECK(totem_esb_prx_next(&queue, 1U << 1U) == -1);
    CHECK(totem_esb_prx_next(&queue, 0U) == 1);
    CHECK(totem_esb_prx_peek(&queue, 1U)->data[0] == 1U);
}

static void prx_fifo_preserves_press_release_order_per_peer(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet packets[] = {
        fixture_packet(1U, 11U, 100U), fixture_packet(1U, 12U, 101U),
        fixture_packet(2U, 21U, 102U), fixture_packet(2U, 22U, 103U),
    };
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        CHECK(totem_esb_prx_offer(&queue, &packets[i]) == 0);
    }
    const uint8_t expected[] = {11U, 21U, 12U, 22U};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        int pipe = totem_esb_prx_next(&queue, 0U);
        CHECK(pipe == (int)(i % 2U + 1U));
        CHECK(totem_esb_prx_peek(&queue, (uint8_t)pipe)->data[0] == expected[i]);
        totem_esb_prx_pop(&queue, (uint8_t)pipe);
    }
    CHECK(totem_esb_prx_next(&queue, 0U) == -1);
}

static void duplicate_prx_frame_does_not_refresh_expiry(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet packet = fixture_packet(1U, 1U, 100U);
    CHECK(totem_esb_prx_offer(&queue, &packet) == 0);
    packet.enqueued_at = 1099U;
    CHECK(totem_esb_prx_offer(&queue, &packet) == 1);
    CHECK(queue.count[1] == 1U);
    CHECK(totem_esb_prx_peek(&queue, 1U)->enqueued_at == 100U);
    CHECK(totem_esb_prx_expire(&queue, 1099U, 1000U) == 0U);
    CHECK(totem_esb_prx_expire(&queue, 1100U, 1000U) == 1U);
    CHECK(totem_esb_prx_peek(&queue, 1U) == NULL);
}

static void fresh_identical_prx_commands_are_not_coalesced(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet first = fixture_packet(1U, 5U, 100U);
    struct totem_esb_prx_packet again = first;
    again.msg_id = 6U;
    again.enqueued_at = 101U;
    CHECK(totem_esb_prx_offer(&queue, &first) == 0);
    CHECK(totem_esb_prx_offer(&queue, &again) == 0);
    CHECK(queue.count[1] == 2U);
    CHECK(totem_esb_prx_offer(&queue, &again) == 1);
    CHECK(queue.count[1] == 2U);
    CHECK(totem_esb_prx_peek(&queue, 1U)->msg_id == 5U);
    totem_esb_prx_pop(&queue, 1U);
    CHECK(totem_esb_prx_peek(&queue, 1U)->msg_id == 6U);

#if TOTEM_ESB_PRX_PIPE_LIMIT >= 3
    /* Legacy v2 commands have no wire sequence: A, B, A are distinct actions. */
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet between = fixture_packet(1U, 6U, 101U);
    again.msg_id = 7U;
    again.enqueued_at = 102U;
    CHECK(totem_esb_prx_offer(&queue, &first) == 0);
    CHECK(totem_esb_prx_offer(&queue, &between) == 0);
    CHECK(totem_esb_prx_offer(&queue, &again) == 0);
    const uint8_t values[] = {5U, 6U, 5U};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        CHECK(totem_esb_prx_peek(&queue, 1U)->data[0] == values[i]);
        CHECK(totem_esb_prx_peek(&queue, 1U)->msg_id == i + 5U);
        totem_esb_prx_pop(&queue, 1U);
    }
    CHECK(queue.count[1] == 0U);
#endif
}

static void prx_expiry_handles_uptime_wrap(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet old = fixture_packet(1U, 1U, UINT32_MAX - 50U);
    struct totem_esb_prx_packet recent = fixture_packet(2U, 2U, 20U);
    CHECK(totem_esb_prx_offer(&queue, &old) == 0);
    CHECK(totem_esb_prx_offer(&queue, &recent) == 0);
    CHECK(totem_esb_prx_expire(&queue, 48U, 100U) == 0U);
    CHECK(totem_esb_prx_expire(&queue, 49U, 100U) == 1U);
    CHECK(totem_esb_prx_peek(&queue, 1U) == NULL);
    CHECK(totem_esb_prx_peek(&queue, 2U)->data[0] == 2U);
}

static void clearing_one_peer_preserves_other_fifo_and_reuses_slots(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet packets[] = {
        fixture_packet(1U, 11U, 100U), fixture_packet(1U, 12U, 101U),
        fixture_packet(2U, 21U, 102U), fixture_packet(2U, 22U, 103U),
    };
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        CHECK(totem_esb_prx_offer(&queue, &packets[i]) == 0);
    }
    totem_esb_prx_clear_pipe(&queue, 1U);
    CHECK(queue.count[1] == 0U && queue.count[2] == 2U);
    CHECK(totem_esb_prx_peek(&queue, 1U) == NULL);
    CHECK(totem_esb_prx_offer(&queue, &packets[0]) == 0);
    CHECK(totem_esb_prx_offer(&queue, &packets[1]) == 0);
    CHECK(totem_esb_prx_peek(&queue, 2U)->data[0] == 21U);
    totem_esb_prx_pop(&queue, 2U);
    CHECK(totem_esb_prx_peek(&queue, 2U)->data[0] == 22U);
    totem_esb_prx_clear_pipe(&queue, 1U);
    totem_esb_prx_clear_pipe(&queue, 2U);
    CHECK(totem_esb_prx_next(&queue, 0U) == -1);
}

static void malformed_prx_frames_leave_queue_unchanged(void) {
    struct totem_esb_prx_queue queue;
    totem_esb_prx_queue_init(&queue);
    struct totem_esb_prx_packet packet = fixture_packet(3U, 1U, 100U);
    CHECK(totem_esb_prx_offer(&queue, &packet) == -EINVAL);
    packet.pipe = 1U;
    packet.length = 0U;
    CHECK(totem_esb_prx_offer(&queue, &packet) == -EINVAL);
    packet.length = CONFIG_ESB_MAX_PAYLOAD_LENGTH + 1U;
    CHECK(totem_esb_prx_offer(&queue, &packet) == -EINVAL);
    CHECK(totem_esb_prx_next(&queue, 0U) == -1);
    CHECK(queue.count[0] == 0U && queue.count[1] == 0U && queue.count[2] == 0U);
}

static const struct {
    const char *name;
    void (*run)(void);
} cases[] = {
    {"missing_release_clears_stuck_key", missing_release_clears_stuck_key},
    {"missing_press_is_recovered", missing_press_is_recovered},
    {"releases_precede_new_presses", releases_precede_new_presses},
    {"held_keys_return_after_receiver_reset", held_keys_return_after_receiver_reset},
    {"peer_states_are_independent", peer_states_are_independent},
    {"bitmap_boundaries_and_padding", bitmap_boundaries_and_padding},
    {"zero_and_full_position_range", zero_and_full_position_range},
    {"malformed_snapshot_is_rejected_before_state_changes", malformed_snapshot_is_rejected_before_state_changes},
    {"exhaustive_one_byte_transitions", exhaustive_one_byte_transitions},
    {"handshake_backoff_grows_caps_and_resets", handshake_backoff_grows_caps_and_resets},
    {"handshake_backoff_saturates_without_integer_wrap", handshake_backoff_saturates_without_integer_wrap},
    {"offline_peer_cannot_starve_active_peer", offline_peer_cannot_starve_active_peer},
    {"prx_fifo_preserves_press_release_order_per_peer", prx_fifo_preserves_press_release_order_per_peer},
    {"duplicate_prx_frame_does_not_refresh_expiry", duplicate_prx_frame_does_not_refresh_expiry},
    {"fresh_identical_prx_commands_are_not_coalesced", fresh_identical_prx_commands_are_not_coalesced},
    {"prx_expiry_handles_uptime_wrap", prx_expiry_handles_uptime_wrap},
    {"clearing_one_peer_preserves_other_fifo_and_reuses_slots", clearing_one_peer_preserves_other_fifo_and_reuses_slots},
    {"malformed_prx_frames_leave_queue_unchanged", malformed_prx_frames_leave_queue_unchanged},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s --list|TEST_NAME\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) {
            puts(cases[i].name);
        } else if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("PASS %s\n", cases[i].name);
            return EXIT_SUCCESS;
        }
    }
    if (strcmp(argv[1], "--list") == 0) {
        return EXIT_SUCCESS;
    }
    fprintf(stderr, "Unknown C regression: %s\n", argv[1]);
    return EXIT_FAILURE;
}
