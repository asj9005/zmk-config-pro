/* SPDX-License-Identifier: MIT
 * Actual central edge/snapshot/cleanup helpers with a synchronous ZMK boundary.
 * The signed activity counter models the non-idempotent +/- behavior of mmv.
 * This does not model hold-tap capture, the scheduler, radio or USB delivery.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <totem/esb_key_state.h>

#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 38
#define LOG_WRN(...) ((void)0)
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

enum { ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT = 1,
       NON_KEY_EVENT = 2 };
struct zmk_split_transport_peripheral_event {
    uint8_t type;
    union {
        struct { uint32_t position; bool pressed; } key_position_event;
        uint32_t other;
    } data;
};
struct zmk_position_state_changed {
    uint8_t source;
    uint32_t position;
    bool state;
    int64_t timestamp;
};
struct transition { uint8_t source; uint32_t position; bool pressed; bool direct; };
static const char *scenario;
static uint8_t key_pos_states[2][5];
static int activity[2][38];
static struct transition transitions[128];
static size_t transition_count;
static size_t other_event_count;
static size_t reset_count[2];
static size_t transitions_at_reset[2];
static int esb_central;
static int64_t now;

static int64_t k_uptime_get(void) { return ++now; }
static void record_transition(uint8_t source, uint32_t position, bool pressed, bool direct) {
    CHECK(source < 2 && position < 38);
    CHECK(transition_count < 128);
    transitions[transition_count++] = (struct transition){source, position, pressed, direct};
    activity[source][position] += pressed ? 1 : -1;
}
static int raise_zmk_position_state_changed(struct zmk_position_state_changed event) {
    CHECK(event.timestamp > 0);
    record_transition(event.source, event.position, event.state, true);
    return 0;
}
static int zmk_split_transport_central_peripheral_event_handler(
    const int *transport, uint8_t source, struct zmk_split_transport_peripheral_event event) {
    CHECK(transport == &esb_central);
    if (event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        record_transition(source, event.data.key_position_event.position,
                          event.data.key_position_event.pressed, false);
    } else {
        CHECK(event.type == NON_KEY_EVENT && event.data.other == 1234U);
        other_event_count++;
    }
    return 0;
}
static void totem_owned_swapper_source_reset(uint8_t source) {
    for (size_t position = 0; position < 38; position++) {
        CHECK(!totem_esb_key_state_get(key_pos_states[source], position));
        CHECK(activity[source][position] == 0);
    }
    reset_count[source]++;
    transitions_at_reset[source] = transition_count;
}

/* ACTUAL_CENTRAL_HELPERS */

static void reset_fixture(const char *name) {
    scenario = name;
    memset(key_pos_states, 0, sizeof(key_pos_states));
    memset(activity, 0, sizeof(activity));
    memset(reset_count, 0, sizeof(reset_count));
    memset(transitions_at_reset, 0, sizeof(transitions_at_reset));
    transition_count = other_event_count = 0;
}
static void wire_key(uint8_t source, uint32_t position, bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };
    dispatch_wire_zmk_event(source, &event);
}
static size_t snapshot(uint8_t source, const uint8_t desired[5]) {
    return totem_esb_key_state_reconcile(key_pos_states[source], desired, 38,
                                         emit_snapshot_key, &source);
}
static void expect_transition(size_t index, uint8_t source, uint32_t position,
                              bool pressed, bool direct) {
    CHECK(index < transition_count);
    struct transition *event = &transitions[index];
    CHECK(event->source == source && event->position == position &&
          event->pressed == pressed && event->direct == direct);
}

static void orphan_release_stays_idle(void) {
    reset_fixture("orphan_release_stays_idle");
    wire_key(0, 0, false);
    wire_key(1, 37, false);
    CHECK(transition_count == 0);
    CHECK(activity[0][0] == 0 && activity[1][37] == 0);
    uint8_t empty[5] = {0};
    CHECK(snapshot(0, empty) == 0 && snapshot(1, empty) == 0);
}
static void normal_edges_and_duplicate_release(void) {
    reset_fixture("normal_edges_and_duplicate_release");
    wire_key(0, 7, true);
    wire_key(0, 7, false);
    wire_key(0, 7, false);
    CHECK(transition_count == 2 && activity[0][7] == 0);
    wire_key(0, 7, true);
    wire_key(0, 7, false);
    CHECK(transition_count == 4 && activity[0][7] == 0);
    for (size_t index = 0; index < 4; index++) {
        expect_transition(index, 0, 7, index % 2 == 0, false);
    }
}
static void repeated_press_keeps_synthetic_release(void) {
    reset_fixture("repeated_press_keeps_synthetic_release");
    wire_key(1, 37, true);
    wire_key(1, 37, true);
    CHECK(transition_count == 3 && activity[1][37] == 1);
    expect_transition(0, 1, 37, true, false);
    expect_transition(1, 1, 37, false, true);
    expect_transition(2, 1, 37, true, false);
    wire_key(1, 37, false);
    wire_key(1, 37, false);
    CHECK(transition_count == 4 && activity[1][37] == 0);
    expect_transition(3, 1, 37, false, false);
}
static void snapshot_recovers_missing_edges(void) {
    reset_fixture("snapshot_recovers_missing_edges");
    uint8_t held[5] = {0};
    uint8_t empty[5] = {0};
    totem_esb_key_state_set(held, 8, true);
    CHECK(snapshot(0, held) == 1 && activity[0][8] == 1);
    CHECK(snapshot(0, held) == 0);
    wire_key(0, 8, false);
    CHECK(activity[0][8] == 0);
    wire_key(0, 8, true);
    CHECK(snapshot(0, empty) == 1 && activity[0][8] == 0);
    wire_key(0, 8, false);
    CHECK(transition_count == 4);
    CHECK(snapshot(0, empty) == 0);
}
static void snapshot_release_order_and_later_press(void) {
    reset_fixture("snapshot_release_order_and_later_press");
    uint8_t desired[5] = {1};
    wire_key(0, 37, true);
    CHECK(snapshot(0, desired) == 2);
    expect_transition(1, 0, 37, false, false);
    expect_transition(2, 0, 0, true, false);
    wire_key(0, 0, true);
    expect_transition(3, 0, 0, false, true);
    expect_transition(4, 0, 0, true, false);
    wire_key(0, 0, false);
    CHECK(activity[0][0] == 0 && activity[0][37] == 0);
}
static void cleanup_and_reconnect_keep_sources_independent(void) {
    reset_fixture("cleanup_and_reconnect_keep_sources_independent");
    wire_key(0, 0, true);
    wire_key(0, 37, true);
    wire_key(1, 8, true);
    release_source_keys(0);
    CHECK(transition_count == 5);
    expect_transition(3, 0, 0, false, true);
    expect_transition(4, 0, 37, false, true);
    CHECK(reset_count[0] == 1 && transitions_at_reset[0] == 5);
    CHECK(activity[1][8] == 1 && totem_esb_key_state_get(key_pos_states[1], 8));
    wire_key(0, 0, false);
    wire_key(0, 37, false);
    CHECK(transition_count == 5);
    uint8_t held[5] = {1};
    CHECK(snapshot(0, held) == 1 && activity[0][0] == 1);
    wire_key(0, 0, false);
    release_source_keys(1);
    CHECK(activity[0][0] == 0 && activity[1][8] == 0);
}
static void non_key_events_are_unchanged(void) {
    reset_fixture("non_key_events_are_unchanged");
    struct zmk_split_transport_peripheral_event event = {.type = NON_KEY_EVENT, .data.other = 1234};
    dispatch_wire_zmk_event(0, &event);
    dispatch_wire_zmk_event(1, &event);
    CHECK(other_event_count == 2 && transition_count == 0);
}
int main(void) {
    orphan_release_stays_idle();
    normal_edges_and_duplicate_release();
    repeated_press_keeps_synthetic_release();
    snapshot_recovers_missing_edges();
    snapshot_release_order_and_later_press();
    cleanup_and_reconnect_keep_sources_independent();
    non_key_events_are_unchanged();
    puts("7 actual central dispatch/snapshot/cleanup scenarios passed");
    return 0;
}
