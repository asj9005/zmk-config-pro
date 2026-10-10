/* SPDX-License-Identifier: MIT
 * Actual packet admission/decode/dequeue and bounded worker with deterministic
 * circular-ring, clock, work and crypto fakes. No real IRQ/PSA/ZMK execution.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include <totem/esb_key_state.h>

#define CONFIG_ESB_MAX_PAYLOAD_LENGTH 64
#define CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT 2
#define CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX 38
#define ZMK_KEYMAP_LEN 38
#define CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS 128
#define CONFIG_ZMK_SPLIT_ESB_CMD_BUFFER_ITEMS 16
#define CONFIG_TOTEM_FIELD_DIAGNOSTICS 1
#define IS_ENABLED(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#if defined(_MSC_VER)
#define __packed
#else
#define __packed __attribute__((packed))
#endif
enum { TOTEM_DIAG_TX_OK, TOTEM_DIAG_TX_FAIL, TOTEM_DIAG_RX,
       TOTEM_DIAG_FRAME_OK, TOTEM_DIAG_FRAME_ERR,
       TOTEM_DIAG_RX_OVERFLOW, TOTEM_DIAG_RX_INVALID_POSITION };
static const char *scenario;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

struct k_work { int unused; };
struct k_spinlock { unsigned int held; };
typedef unsigned int k_spinlock_key_t;
static unsigned int locks;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    CHECK(!lock->held && locks == 0); lock->held = 1; locks++; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    ARG_UNUSED(key); CHECK(lock->held && locks == 1); lock->held = 0; locks--;
}
#if !TEST_CENTRAL && CONFIG_TOTEM_ESB_V3
#define K_FOREVER (-1)
static struct { bool held; } event_mutex;
static unsigned int mutex_locks, mutex_unlocks, key_generation;
static bool inject_rekey, rekey_waiting, decrypt_completed;
static void k_mutex_lock(void *mutex, int timeout) {
    CHECK(mutex == &event_mutex && timeout == K_FOREVER && locks == 0);
    CHECK(!event_mutex.held); event_mutex.held = true; mutex_locks++;
}
static void k_mutex_unlock(void *mutex) {
    CHECK(mutex == &event_mutex && event_mutex.held && locks == 0);
    event_mutex.held = false; mutex_unlocks++;
    if (rekey_waiting) {
        /* A producer waiting to retire the selected key resumes here. */
        CHECK(decrypt_completed); key_generation++; rekey_waiting = false;
    }
}
#endif
static int64_t now;
enum { TOTEM_FIELD_RX_QUEUE, TOTEM_FIELD_RX_PROCESS };
static uint64_t field_clock, field_last[2];
static unsigned int field_count[2];
static uint64_t totem_field_now_us(void) { field_clock += 25; return field_clock; }
static void totem_field_observe(int metric, uint64_t duration_us) {
    CHECK(locks == 0 && metric >= 0 && metric < 2);
    field_last[metric] = duration_us; field_count[metric]++;
}
static uint32_t k_uptime_get_32(void) { return (uint32_t)now; }
static int64_t k_uptime_get(void) { return now; }
struct ring_buf { uint8_t *buffer; uint32_t capacity, size, head; };
static void ring_buf_init(struct ring_buf *r, uint32_t capacity, uint8_t *data) {
    *r = (struct ring_buf){.buffer = data, .capacity = capacity};
}
static uint32_t ring_buf_size_get(const struct ring_buf *r) { return r->size; }
static uint32_t ring_buf_space_get(const struct ring_buf *r) { return r->capacity - r->size; }
static bool ring_buf_is_empty(const struct ring_buf *r) { return r->size == 0; }
static void ring_buf_reset(struct ring_buf *r) { r->head = r->size = 0; }
static uint32_t ring_buf_put(struct ring_buf *r, const uint8_t *data, uint32_t size) {
    CHECK(r->buffer != NULL && size <= ring_buf_space_get(r));
    for (uint32_t i = 0; i < size; i++) { r->buffer[(r->head + r->size + i) % r->capacity] = data[i]; }
    r->size += size; return size;
}
static uint32_t ring_buf_peek(const struct ring_buf *r, uint8_t *data, uint32_t size) {
    if (size > r->size) { size = r->size; }
    for (uint32_t i = 0; i < size; i++) { data[i] = r->buffer[(r->head + i) % r->capacity]; }
    return size;
}
static uint32_t ring_buf_get(struct ring_buf *r, uint8_t *data, uint32_t size) {
    size = ring_buf_peek(r, data, size); r->head = (r->head + size) % r->capacity; r->size -= size; return size;
}

/* Minimal public transport types at the parser boundary, not ZMK behaviors. */
#pragma pack(push, 1)
enum { ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
       ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
       ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
       ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT };
struct zmk_split_transport_peripheral_event {
    uint8_t type;
    union {
        struct { uint8_t position; bool pressed; } key_position_event;
        uint32_t input_event, sensor_event, battery_event;
    } data;
} __packed;
struct zmk_split_transport_central_command { uint8_t type; uint32_t data; } __packed;
struct totem_esb_link_metric_payload { uint8_t metric; uint32_t value; } __packed;
#define TOTEM_ESB_LINK_METRIC_COUNT 10
/* ACTUAL_WIRE_TYPES */
/* ACTUAL_RX_RECORD */
#pragma pack(pop)
/* ACTUAL_APP_EVENT_TYPES */
/* ACTUAL_BATCH_SIZE */
typedef void (*zmk_split_esb_process_rx_callback_t)(uint8_t pipe);
/* ACTUAL_TRANSPORT_STATE */
/* ACTUAL_RX_STORAGE */
/* ACTUAL_RX_INIT */
static struct ring_buf tx_buf;
static unsigned int scheduled, invalid_count, overflow_count, frame_errors, worker_errors, bad_positions;
static uint32_t high_water, max_age;
static void process_rx_cb(uint8_t pipe) { ARG_UNUSED(pipe); CHECK(locks == 0); scheduled++; }
/* ACTUAL_STATE_INIT */
static void zmk_split_esb_tx(struct zmk_split_esb_state *s) { ARG_UNUSED(s); }
static void totem_esb_diag_event(int event, int result) {
    CHECK(locks == 0); ARG_UNUSED(result);
    if (event == TOTEM_DIAG_FRAME_ERR) { frame_errors++; }
    if (event == TOTEM_DIAG_RX_INVALID_POSITION) { bad_positions++; }
}
static void totem_esb_diag_rx_observe(uint32_t bytes, uint32_t age) {
    CHECK(locks == 0); if (bytes > high_water) { high_water = bytes; } if (age > max_age) { max_age = age; }
}
static void totem_esb_benchmark_tx(uint8_t source, uint16_t id, uint16_t attempts, bool ok) {
    ARG_UNUSED(source); ARG_UNUSED(id); ARG_UNUSED(attempts); ARG_UNUSED(ok);
}
static void totem_esb_benchmark_rx_invalid(uint8_t pipe, int err) {
    ARG_UNUSED(pipe); CHECK(err < 0); invalid_count++;
}
static void totem_esb_benchmark_rx_overflow(uint8_t pipe, uint32_t count) {
    CHECK(locks == 0); CHECK(pipe < CONFIG_ESB_PIPE_COUNT && count > 0); overflow_count++;
}
static uint32_t crc32_ieee(const uint8_t *data, size_t size) {
    /* Deliberate deterministic fake; this test is not a CRC algorithm test. */
    uint32_t value = 17; for (size_t i = 0; i < size; i++) { value = value * 33U + data[i]; } return value;
}
#if CONFIG_TOTEM_ESB_V3
enum totem_esb_v3_key_stage { TOTEM_ESB_V3_ACTIVE_KEY, TOTEM_ESB_V3_ROOT_KEY, TOTEM_ESB_V3_PENDING_KEY };
enum { TOTEM_ESB_V3_DOWNLINK, TOTEM_ESB_V3_UPLINK };
static enum totem_esb_v3_key_stage last_stage;
static unsigned int crypto_calls;
static int totem_esb_v3_open(uint8_t source, int direction, enum totem_esb_v3_key_stage stage,
                             uint64_t session, uint32_t sequence, uint8_t *aad, size_t aad_len,
                             uint8_t *body, size_t body_len, const uint8_t *tag) {
    CHECK(locks == 0 && source < CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT);
    CHECK(aad_len == sizeof(struct esb_msg_prefix) + sizeof(struct esb_v3_wire_payload_header));
    CHECK(body == aad + aad_len); ARG_UNUSED(body_len); ARG_UNUSED(direction); ARG_UNUSED(session); ARG_UNUSED(sequence);
#if !TEST_CENTRAL
    if (inject_rekey) {
        unsigned int selected_key = key_generation;
        inject_rekey = false;
        /* Model a PSA operation yielding after it selects the old handle.
         * A concurrent producer takes event_mutex before retiring that key. */
        if (event_mutex.held) { rekey_waiting = true; }
        else { key_generation++; }
        if (key_generation != selected_key) { return -EIO; }
        decrypt_completed = true;
    }
#endif
    last_stage = stage; crypto_calls++; return tag[0] == 0xee ? -EACCES : 0;
}
static void totem_esb_benchmark_security_drop(uint8_t source, const char *why, uint32_t sequence) {
    ARG_UNUSED(source); ARG_UNUSED(why); ARG_UNUSED(sequence);
}
#endif
/* ACTUAL_RX_FUNCTIONS */
/* ACTUAL_POSITION_VALIDATION */

#if TEST_CENTRAL
typedef struct esb_event_envelope test_envelope;
#else
typedef struct esb_command_envelope test_envelope;
#endif
static uint8_t first_pipe(void) {
#if TEST_CENTRAL
    return 1;
#else
    return peripheral_id;
#endif
}
static size_t make_packet(uint8_t *packet, uint8_t pipe, uint32_t id, uint8_t wire_type) {
    test_envelope env = {0};
    memcpy(env.prefix.magic_prefix, ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX, sizeof(env.prefix.magic_prefix));
    env.prefix.payload_size = sizeof(env.payload);
    env.payload.source = pipe;
#if CONFIG_TOTEM_ESB_V3 || TEST_CENTRAL
    env.payload.wire_type = wire_type; env.payload.sequence = id; env.payload.session_id = 123;
#else
    ARG_UNUSED(wire_type); env.payload.cmd.data = id;
#endif
    memcpy(packet, &env, sizeof(env));
    size_t length = sizeof(env);
#if ESB_MSG_HAS_POSTFIX
    struct esb_msg_postfix postfix = {0};
#if !CONFIG_TOTEM_ESB_V3
    postfix.crc = crc32_ieee(packet, length);
#endif
    memcpy(packet + length, &postfix, sizeof(postfix)); length += sizeof(postfix);
#endif
    CHECK(length <= CONFIG_ESB_MAX_PAYLOAD_LENGTH); return length;
}
static void admit(uint8_t pipe, const uint8_t *packet, size_t size) {
    app_esb_event_t event = {.evt_type = APP_ESB_EVT_RX, .pipe = pipe, .buf = (uint8_t *)packet, .data_length = size};
    zmk_split_esb_cb(&event, &state);
}
static void enqueue_id(uint8_t pipe, uint32_t id) {
    uint8_t packet[CONFIG_ESB_MAX_PAYLOAD_LENGTH]; size_t size = make_packet(packet, pipe, id, 0); admit(pipe, packet, size);
}
static uint32_t envelope_id(const test_envelope *env) {
#if CONFIG_TOTEM_ESB_V3 || TEST_CENTRAL
    return env->payload.sequence;
#else
    return env->payload.cmd.data;
#endif
}
static uint32_t delivered[512];
static uint8_t delivered_pipes[512];
static unsigned int delivered_count;
static bool replenish;
static void record_dispatch(uint8_t pipe, const test_envelope *env) {
    CHECK(locks == 0 && delivered_count < 512);
#if !TEST_CENTRAL && CONFIG_TOTEM_ESB_V3
    CHECK(!event_mutex.held);
#endif
    delivered_pipes[delivered_count] = pipe; delivered[delivered_count++] = envelope_id(env);
    if (replenish && delivered_count < 32) { enqueue_id(first_pipe(), 1000 + delivered_count); }
}
/* ACTUAL_WORKER */
static int pop(test_envelope *env, uint8_t *pipe) {
    return zmk_split_esb_rx_get(&state, (uint8_t *)env, sizeof(*env), !TEST_CENTRAL, pipe, NULL);
}
static void reset_fixture(const char *name) {
    scenario = name; init_rx_buffers(); memset(state.rx_pipe_bytes, 0, sizeof(state.rx_pipe_bytes));
    memset(state.rx_overflow_count, 0, sizeof(state.rx_overflow_count));
    scheduled = invalid_count = overflow_count = frame_errors = worker_errors = bad_positions = 0;
    delivered_count = 0; replenish = false; now = high_water = max_age = 0; CHECK(locks == 0);
    field_clock = 0;
    memset(field_last, 0, sizeof(field_last)); memset(field_count, 0, sizeof(field_count));
#if !TEST_CENTRAL && CONFIG_TOTEM_ESB_V3
    CHECK(!event_mutex.held && !rekey_waiting);
    mutex_locks = mutex_unlocks = key_generation = 0;
    inject_rekey = decrypt_completed = false;
#endif
}

static void rx_rekey_exclusion(void) {
#if !TEST_CENTRAL && CONFIG_TOTEM_ESB_V3
    /* References also keep the deliberately missing-lock mutation compilable. */
    (void)k_mutex_lock; (void)k_mutex_unlock;
    reset_fixture("rx_rekey_exclusion");
    enqueue_id(first_pipe(), 81); inject_rekey = true;
    struct k_work work = {0}; process_rx_work_cb(&work);
    CHECK(decrypt_completed && key_generation == 1 && !rekey_waiting);
    CHECK(delivered_count == 1 && delivered[0] == 81 && worker_errors == 0);
    CHECK(!event_mutex.held && mutex_locks == 2 && mutex_unlocks == 2);
    /* Authentication, malformed packets, and an empty queue must all unlock. */
    reset_fixture("rx_rekey_error_unlock");
    uint8_t packet[64]; size_t size = make_packet(packet, first_pipe(), 82, 0);
    packet[size - sizeof(struct esb_msg_postfix)] = 0xee;
    admit(first_pipe(), packet, size);
    size = make_packet(packet, first_pipe(), 83, 0); packet[0] ^= 1;
    admit(first_pipe(), packet, size);
    process_rx_work_cb(&work);
    CHECK(worker_errors == 2 && delivered_count == 0);
    CHECK(!event_mutex.held && mutex_locks == 3 && mutex_unlocks == 3);
#endif
}

static void storage_and_admission(void) {
    reset_fixture("storage_and_admission");
    CHECK(sizeof(struct esb_rx_record) == 6);
    CHECK(sizeof(rx_buf_data) == RX_RING_BUF_SIZE * (TEST_CENTRAL ? CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT : 1));
    uint8_t packet[CONFIG_ESB_MAX_PAYLOAD_LENGTH + 1] = {0};
    for (unsigned int pipe = 0; pipe <= CONFIG_ESB_PIPE_COUNT; pipe++) {
        size_t size = make_packet(packet, (uint8_t)pipe, pipe + 1, 0);
        unsigned int before = scheduled;
        admit((uint8_t)pipe, packet, size);
        bool allowed = pipe >= state.rx_first_pipe && pipe <= state.rx_last_pipe;
        CHECK(scheduled == before + (unsigned int)allowed);
    }
    unsigned int before = invalid_count; admit(UINT8_MAX, packet, 10); CHECK(invalid_count == before + 1);
    before = scheduled; admit(first_pipe(), packet, 0); admit(first_pipe(), packet, sizeof(packet)); CHECK(scheduled == before);
    struct k_work work = {0}; process_rx_work_cb(&work);
    CHECK(delivered_count == (TEST_CENTRAL ? 2U : 1U));
}
static void arrival_order(void) {
    reset_fixture("arrival_order");
    uint8_t a = first_pipe(), b = TEST_CENTRAL ? 2 : a;
    enqueue_id(b, 11); enqueue_id(a, 12); enqueue_id(b, 13);
    struct k_work work = {0}; process_rx_work_cb(&work);
    CHECK(delivered_count == 3 && delivered[0] == 11 && delivered[1] == 12 && delivered[2] == 13);
    CHECK(delivered_pipes[0] == b && delivered_pipes[1] == a && delivered_pipes[2] == b);
}
static void per_pipe_quota(void) {
    reset_fixture("per_pipe_quota");
    uint8_t packet[64]; size_t size = make_packet(packet, first_pipe(), 5, 0);
    size_t count = RX_RING_BUF_SIZE / (sizeof(struct esb_rx_record) + size);
    for (size_t i = 0; i < count; i++) { admit(first_pipe(), packet, size); }
    CHECK(scheduled == count); admit(first_pipe(), packet, size);
    CHECK(scheduled == count && overflow_count == 1);
#if TEST_CENTRAL
    enqueue_id(2, 6); CHECK(scheduled == count + 1);
#endif
    test_envelope env; uint8_t pipe;
    for (size_t i = 0; i < count; i++) { CHECK(pop(&env, &pipe) == 0 && pipe == first_pipe()); }
#if TEST_CENTRAL
    CHECK(pop(&env, &pipe) == 0 && pipe == 2);
#endif
    CHECK(pop(&env, &pipe) == -ENODATA);
    enqueue_id(first_pipe(), 7); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 7);
}
static void bounded_worker(void) {
    reset_fixture("bounded_worker");
    enqueue_id(first_pipe(), 10); enqueue_id(TEST_CENTRAL ? 2 : first_pipe(), 11);
    replenish = true; unsigned int before = scheduled;
    struct k_work work = {0}; process_rx_work_cb(&work);
    CHECK(delivered_count == ESB_RX_WORK_BATCH_SIZE);
    CHECK(delivered[1] == 11); CHECK(scheduled == before + ESB_RX_WORK_BATCH_SIZE + 1);
    replenish = false; process_rx_work_cb(&work); CHECK(delivered_count == ESB_RX_WORK_BATCH_SIZE + 2);
    before = scheduled; process_rx_work_cb(&work); CHECK(scheduled == before);
}
static void packet_boundaries(void) {
    reset_fixture("packet_boundaries");
    uint8_t packet[65]; test_envelope env; uint8_t pipe;
    size_t size = make_packet(packet, first_pipe(), 1, 0);
    packet[0] ^= 1; admit(first_pipe(), packet, size); enqueue_id(first_pipe(), 2);
    CHECK(pop(&env, &pipe) == -EPROTO); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 2);
    size = make_packet(packet, first_pipe(), 3, 0);
    admit(first_pipe(), packet, size - 1); enqueue_id(first_pipe(), 4);
    CHECK(pop(&env, &pipe) == -EMSGSIZE); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 4);
    size = make_packet(packet, first_pipe(), 5, 0); packet[size] = 0;
    admit(first_pipe(), packet, size + 1); enqueue_id(first_pipe(), 6);
    CHECK(pop(&env, &pipe) == -EMSGSIZE); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 6);
    size = make_packet(packet, first_pipe(), 7, 0); packet[4] = UINT8_MAX;
    admit(first_pipe(), packet, size); enqueue_id(first_pipe(), 8);
    CHECK(pop(&env, &pipe) == -EMSGSIZE); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 8);
    CHECK(frame_errors == 4);
}
static void authentication_boundary(void) {
    reset_fixture("authentication_boundary");
    uint8_t packet[64]; test_envelope env; uint8_t pipe;
    size_t size = make_packet(packet, first_pipe(), 1, 0);
#if CONFIG_TOTEM_ESB_V3
    packet[size - sizeof(struct esb_msg_postfix)] = 0xee;
    admit(first_pipe(), packet, size); enqueue_id(first_pipe(), 2);
    CHECK(pop(&env, &pipe) == -EACCES); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 2);
    size = make_packet(packet, 0, 3, 0); admit(first_pipe(), packet, size);
    unsigned int before = crypto_calls; CHECK(pop(&env, &pipe) == -EADDRNOTAVAIL && crypto_calls == before);
    size = make_packet(packet, first_pipe() == 1 ? 2 : 1, 4, 0); admit(first_pipe(), packet, size);
    CHECK(pop(&env, &pipe) == -EADDRNOTAVAIL && crypto_calls == before);
    uint8_t types[] = {0, TEST_CENTRAL ? ESB_WIRE_EVENT_V3_HELLO : ESB_WIRE_COMMAND_V3_CHALLENGE,
                      TEST_CENTRAL ? ESB_WIRE_EVENT_V3_READY : ESB_WIRE_COMMAND_V3_SESSION_OK};
    enum totem_esb_v3_key_stage stages[] = {TOTEM_ESB_V3_ACTIVE_KEY, TOTEM_ESB_V3_ROOT_KEY, TOTEM_ESB_V3_PENDING_KEY};
    for (size_t i = 0; i < 3; i++) {
        size = make_packet(packet, first_pipe(), 5, types[i]); admit(first_pipe(), packet, size);
        CHECK(pop(&env, &pipe) == 0 && last_stage == stages[i]);
    }
#elif ESB_MSG_HAS_POSTFIX
    packet[size - 1] ^= 1; admit(first_pipe(), packet, size); enqueue_id(first_pipe(), 2);
    CHECK(pop(&env, &pipe) == -EBADMSG); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == 2);
#else
    admit(first_pipe(), packet, size); CHECK(pop(&env, &pipe) == 0);
#endif
}
static void wrap_and_age(void) {
    reset_fixture("wrap_and_age");
    test_envelope env; uint8_t pipe;
    for (unsigned int i = 0; i < 600; i++) {
        now = i; enqueue_id(first_pipe(), i); CHECK(pop(&env, &pipe) == 0 && envelope_id(&env) == i);
    }
    now = UINT32_MAX - 9U; enqueue_id(first_pipe(), 601); now = (int64_t)UINT32_MAX + 16;
    CHECK(pop(&env, &pipe) == 0 && max_age == 25);
    CHECK(field_count[TOTEM_FIELD_RX_QUEUE] == 601);
    CHECK(field_count[TOTEM_FIELD_RX_PROCESS] == 601);
    CHECK(field_last[TOTEM_FIELD_RX_QUEUE] == 25000);
    CHECK(field_last[TOTEM_FIELD_RX_PROCESS] == 25);
    CHECK(high_water > sizeof(struct esb_rx_record));
}
static void position_validation(void) {
    reset_fixture("position_validation");
    struct esb_event_envelope env = {0};
    env.payload.wire_type = ESB_WIRE_EVENT_ZMK;
    env.payload.body.event.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT;
    env.prefix.payload_size = offsetof(struct esb_event_payload, body) + sizeof(env.payload.body.event.type) +
                              sizeof(env.payload.body.event.data.key_position_event);
    env.payload.body.event.data.key_position_event.position = 37;
    CHECK(event_payload_size_is_valid(&env));
    env.payload.body.event.data.key_position_event.position = 38; CHECK(!event_payload_size_is_valid(&env));
    env.payload.body.event.data.key_position_event.position = 255; CHECK(!event_payload_size_is_valid(&env));
    CHECK(bad_positions == 2);
}
static void ingress_time_and_timer_barrier(void) {
    reset_fixture("ingress_time_and_timer_barrier");
    test_envelope env; uint8_t pipe; int64_t received_at = -1;
    now = (int64_t)UINT32_MAX - 9; enqueue_id(first_pipe(), 701);
    now += 40;
    CHECK(zmk_split_esb_rx_pending_before(&state, (int64_t)UINT32_MAX - 9));
    CHECK(!zmk_split_esb_rx_pending_before(&state, (int64_t)UINT32_MAX - 10));
    CHECK(zmk_split_esb_rx_get(&state, (uint8_t *)&env, sizeof(env), !TEST_CENTRAL,
                              &pipe, &received_at) == 0);
    CHECK(received_at == (int64_t)UINT32_MAX - 9 && envelope_id(&env) == 701);
    CHECK(!zmk_split_esb_rx_pending_before(&state, now));
    /* Later traffic never prolongs the older tapping deadline. */
    enqueue_id(first_pipe(), 702);
    CHECK(!zmk_split_esb_rx_pending_before(&state, now - 1));
    CHECK(pop(&env, &pipe) == 0);
    /* Malformed local metadata is cleared once rather than holding a timer. */
    enqueue_id(first_pipe(), 703);
    rx_buf.buffer[(rx_buf.head + offsetof(struct esb_rx_record, length)) % rx_buf.capacity] = 0;
    unsigned int before = frame_errors;
    CHECK(!zmk_split_esb_rx_pending_before(&state, now));
    CHECK(frame_errors == before + 1 && ring_buf_is_empty(&rx_buf));
    for (size_t p = 0; p < CONFIG_ESB_PIPE_COUNT; p++) { CHECK(state.rx_pipe_bytes[p] == 0); }
    CHECK(!zmk_split_esb_rx_pending_before(&state, now) && frame_errors == before + 1);
    /* Malformed RF payload keeps valid metadata: its normal dequeue releases
     * the barrier without dispatching the unauthenticated event. */
    uint8_t packet[64]; size_t size = make_packet(packet, first_pipe(), 704, 0);
    packet[0] ^= 1; admit(first_pipe(), packet, size);
    CHECK(zmk_split_esb_rx_pending_before(&state, now));
    CHECK(pop(&env, &pipe) == -EPROTO);
    CHECK(!zmk_split_esb_rx_pending_before(&state, now));
}
struct esb_payload { uint8_t pipe; uint16_t length; uint8_t data[64]; };
static struct esb_payload radio_packets[2];
static unsigned int radio_index;
static uint8_t *driver_payload_data;
static int esb_read_rx_payload(struct esb_payload *payload) {
    driver_payload_data = payload->data;
    if (radio_index == 2) { memset(payload, 0xa5, sizeof(*payload)); return -ENODATA; }
    *payload = radio_packets[radio_index++]; return 0;
}
static void m_callback(app_esb_event_t *event) {
    CHECK(event->buf == driver_payload_data); zmk_split_esb_cb(event, &state);
}
/* ACTUAL_RADIO_DISPATCH */
static void driver_buffer_lifetime(void) {
    reset_fixture("driver_buffer_lifetime"); radio_index = 0;
    for (unsigned int i = 0; i < 2; i++) {
        radio_packets[i].pipe = first_pipe();
        radio_packets[i].length = make_packet(radio_packets[i].data, first_pipe(), 71 + i, 0);
    }
    receive_from_radio();
    struct k_work work = {0}; process_rx_work_cb(&work);
    CHECK(delivered_count == 2 && delivered[0] == 71 && delivered[1] == 72);
}
int main(void) {
    /* Keep the CRC fake referenced in all configurations without changing the parser. */
    CHECK(crc32_ieee((const uint8_t *)"", 0) == 17);
    storage_and_admission(); arrival_order(); per_pipe_quota(); bounded_worker();
    packet_boundaries(); authentication_boundary(); wrap_and_age(); position_validation(); driver_buffer_lifetime();
    rx_rekey_exclusion();
    ingress_time_and_timer_barrier();
    puts("11 actual RX scenarios passed"); return 0;
}
