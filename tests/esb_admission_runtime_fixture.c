/* SPDX-License-Identifier: MIT
 * Actual v3 frame producer with bounded ring, locks and crypto-call fakes.
 * Checks admission/sequence commits, not the PSA cipher or radio hardware.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#if __has_warning("-Wunterminated-string-initialization")
/* The production wire prefix intentionally occupies four bytes without NUL;
 * C permits that string initializer, but recent Clang warns with -Wextra. */
#pragma clang diagnostic ignored "-Wunterminated-string-initialization"
#endif
#endif

#define IS_ENABLED(option) (option)
#define CONFIG_TOTEM_ESB_V3 1
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 38
#define CONFIG_ZMK_SPLIT_ESB_RETRY_KEY_POSITION 3
#define TOTEM_ESB_LINK_METRIC_COUNT 3U
#define ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX "ZmK3"
#define ESB_KEY_STATE_BYTES ((CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX + 7) / 8)
#define __packed __attribute__((packed))
#define K_FOREVER (-1)
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #value); exit(1); \
} } while (0)

typedef ptrdiff_t ssize_t;
struct zmk_split_transport_peripheral_event { int type; uint8_t data[8]; };
struct totem_esb_link_metric_payload { uint8_t metric; uint32_t value; };
/* ACTUAL_FRAME_STRUCTURES */
enum { ESB_V3_WAIT_CHALLENGE, ESB_V3_ESTABLISHED };
struct k_spinlock { unsigned int depth; };
typedef unsigned int k_spinlock_key_t;
static struct k_spinlock tx_ring_lock;
static int event_mutex;
static struct { uint8_t bytes[256]; size_t used; } tx_buf;
static struct { uint8_t tx_pipe; } state = {.tx_pipe = 1};
static const uint8_t peripheral_id = 1;
static struct esb_key_state_payload local_key_state;
static uint64_t peripheral_nonce, wire_session_id;
static uint32_t root_sequence, wire_sequence, last_probe_sequence;
static uint8_t heartbeat_metric;
static unsigned int finalize_calls, pump_calls, pressure_calls;
static int secure_state, finalize_result;
static bool fill_during_finalize;
static struct esb_event_envelope sealed;
static const char *scenario;

static void k_mutex_lock(int *mutex, int timeout) {
    CHECK(mutex == &event_mutex && timeout == K_FOREVER && *mutex == 0);
    (*mutex)++;
}
static void k_mutex_unlock(int *mutex) {
    CHECK(mutex == &event_mutex && *mutex == 1); (*mutex)--;
}
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    CHECK(lock == &tx_ring_lock && event_mutex == 1 && lock->depth == 0);
    lock->depth++; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    (void)key;
    CHECK(lock == &tx_ring_lock && lock->depth == 1); lock->depth--;
}
static int get_secure_state(void) { return secure_state; }
static ssize_t get_payload_data_size(const struct zmk_split_transport_peripheral_event *event) {
    return event->type == 1 ? (ssize_t)sizeof(event->data) : -ENOTSUP;
}
static uint8_t get_retry_count(const struct zmk_split_transport_peripheral_event *event) {
    (void)event; return 3;
}
static uint32_t k_cycle_get_32(void) { return 123U; }
static uint32_t totem_esb_link_metric_value(uint8_t metric) { return metric; }
static size_t ring_buf_space_get(const void *ring) {
    CHECK(ring == &tx_buf && tx_ring_lock.depth == 1);
    return sizeof(tx_buf.bytes) - tx_buf.used;
}
static size_t ring_buf_put(void *ring, const uint8_t *data, size_t size) {
    CHECK(ring == &tx_buf && size <= ring_buf_space_get(ring));
    memcpy(&tx_buf.bytes[tx_buf.used], data, size); tx_buf.used += size; return size;
}
static void ring_buf_reset(void *ring) { CHECK(ring == &tx_buf); tx_buf.used = 0; }
static void totem_esb_transport_queue_pressure(bool producer) {
    CHECK(producer && tx_ring_lock.depth == 1); pressure_calls++;
}
static void begin_tx(void) { CHECK(tx_ring_lock.depth == 1); pump_calls++; }
static int zmk_split_esb_finalize_item(uint8_t *env, size_t length,
                                      bool downlink, struct esb_msg_postfix *postfix) {
    CHECK(event_mutex == 1 && tx_ring_lock.depth == 0 && !downlink);
    CHECK(length <= sizeof(sealed));
    finalize_calls++;
    memset(&sealed, 0, sizeof(sealed));
    memcpy(&sealed, env, length);
    memset(postfix, 0xa5, sizeof(*postfix));
    if (fill_during_finalize) { tx_buf.used = sizeof(tx_buf.bytes); }
    return finalize_result;
}

/* ACTUAL_V3_ENQUEUE */

static void reset(const char *name) {
    scenario = name;
    CHECK(event_mutex == 0 && tx_ring_lock.depth == 0);
    memset(&tx_buf, 0, sizeof(tx_buf));
    peripheral_nonce = 31; wire_session_id = 42;
    root_sequence = 11; wire_sequence = 7; last_probe_sequence = 11;
    heartbeat_metric = 0; secure_state = ESB_V3_ESTABLISHED;
    finalize_calls = pump_calls = pressure_calls = 0;
    finalize_result = 0; fill_during_finalize = false;
}
static void counters_unchanged(void) {
    CHECK(root_sequence == 11 && wire_sequence == 7 && last_probe_sequence == 11);
    CHECK(heartbeat_metric == 0 && event_mutex == 0 && tx_ring_lock.depth == 0);
}
static void full_ring_skips_crypto_and_keeps_sequence(void) {
    reset("full_ring_skips_crypto");
    struct zmk_split_transport_peripheral_event event = {.type = 1};
    tx_buf.used = sizeof(tx_buf.bytes);
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event) == -ENOSPC);
    CHECK(finalize_calls == 0 && pump_calls == 1 && pressure_calls == 1);
    counters_unchanged();
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_V3_RECOVERY, NULL) == -ENOSPC);
    CHECK(finalize_calls == 0 && pump_calls == 2 && pressure_calls == 2);
    counters_unchanged();
    tx_buf.used = 0;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event) == 0);
    CHECK(finalize_calls == 1 && wire_sequence == 8 && root_sequence == 11);
    CHECK(sealed.payload.sequence == 8 && sealed.payload.session_id == wire_session_id);
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_V3_RECOVERY, NULL) == 0);
    CHECK(finalize_calls == 2 && root_sequence == 12 && last_probe_sequence == 12);
    CHECK(heartbeat_metric == 1 && sealed.payload.sequence == 12);
}
static void validation_precedes_capacity(void) {
    reset("validation_precedes_capacity");
    struct zmk_split_transport_peripheral_event event = {.type = 1};
    tx_buf.used = sizeof(tx_buf.bytes);
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, NULL) == -EINVAL);
    event.type = 99;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event) == -ENOTSUP);
    CHECK(enqueue_v3_frame((enum esb_wire_event_type)99, NULL) == -ENOTSUP);
    event.type = 1; secure_state = ESB_V3_WAIT_CHALLENGE;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event) == -ENOTCONN);
    secure_state = ESB_V3_ESTABLISHED; wire_sequence = UINT32_MAX - 1U;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_ZMK, &event) == -EOVERFLOW);
    root_sequence = UINT32_MAX - 1U;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_V3_RECOVERY, NULL) == -EOVERFLOW);
    CHECK(finalize_calls == 0 && pump_calls == 0 && event_mutex == 0);
}
static void crypto_failure_cannot_commit(void) {
    reset("crypto_failure_cannot_commit");
    finalize_result = -EIO;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_KEY_STATE, NULL) == -EIO);
    CHECK(finalize_calls == 1 && tx_buf.used == 0 && pump_calls == 0);
    counters_unchanged();
}
static void final_capacity_guard_remains(void) {
    reset("final_capacity_guard_remains");
    fill_during_finalize = true;
    CHECK(enqueue_v3_frame(ESB_WIRE_EVENT_KEY_STATE, NULL) == -ENOSPC);
    CHECK(finalize_calls == 1 && pump_calls == 1 && pressure_calls == 1);
    counters_unchanged();
}
int main(void) {
    full_ring_skips_crypto_and_keeps_sequence();
    validation_precedes_capacity();
    crypto_failure_cannot_commit();
    final_capacity_guard_remains();
    puts("4 v3 producer admission cases passed");
    return 0;
}
