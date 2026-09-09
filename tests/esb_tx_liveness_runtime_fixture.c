/* SPDX-License-Identifier: MIT
 * Actual peripheral full-ring admission, common TX and app PTX send/pull.
 * Fakes cover sealed frame bytes, bounded queues, locks and driver outcomes.
 * Crypto, IRQ timing and the origin of a driver failure are outside this test.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_LOWER_CAPACITY
#define TEST_LOWER_CAPACITY 4
#endif
#ifndef TEST_PRODUCER_CAPACITY
#define TEST_PRODUCER_CAPACITY 3
#endif
#define CONFIG_ESB_MAX_PAYLOAD_LENGTH 64
#define CONFIG_ESB_PIPE_COUNT 3
#define CONFIG_ZMK_SPLIT_ESB_PROTO_TX_ACK 1
#define CONFIG_ZMK_SPLIT_ESB_USE_TIMESLOT 0
#define CONFIG_ZMK_SPLIT_ESB_MSGQ_FULL_TIMEOUT_MS 3000
#define IS_ENABLED(value) (value)
#define ESB_MSG_HAS_POSTFIX 1
#define ZMK_SPLIT_ESB_ENVELOPE_MAGIC_PREFIX "ZmK3"
#define __packed __attribute__((packed))
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", scenario, __LINE__, #value); exit(1); \
} } while (0)
static const char *scenario;

enum { APP_ESB_MODE_PTX, APP_ESB_MODE_PRX };
enum { TOTEM_DIAG_TX_SEND, TOTEM_DIAG_TX_WRITE, TOTEM_DIAG_TX_START,
       TOTEM_DIAG_HF_WAIT, TOTEM_DIAG_RADIO_BUSY };
struct esb_payload { uint8_t pipe; bool noack; uint16_t length; uint8_t data[64]; };
/* ACTUAL_FRAME_STRUCTURES */
struct queued_payload { struct esb_payload payload; uint16_t msg_id; };
typedef struct {
    uint8_t pipe; uint8_t *data; uint32_t len; uint16_t msg_id; uint8_t max_retry;
} app_esb_data_t;
enum { RADIO_BYTES = sizeof(struct esb_msg_prefix) + 8 + sizeof(struct esb_msg_postfix),
       FRAME_BYTES = RADIO_BYTES + sizeof(struct esb_msg_meta) };

struct ring_buf {
    uint8_t bytes[TEST_PRODUCER_CAPACITY * FRAME_BYTES];
    size_t size, claimed;
};
struct k_spinlock { unsigned int depth; };
typedef unsigned int k_spinlock_key_t;
static struct k_spinlock tx_ring_lock, m_tx_lock;
static int event_mutex;
static struct ring_buf tx_buf;
static struct zmk_split_esb_state { struct ring_buf *tx_buf; } state = {.tx_buf = &tx_buf};
static struct {
    struct queued_payload items[TEST_LOWER_CAPACITY]; size_t size;
} m_msgq_tx_payloads;
static bool m_active = true, m_hf_ready = true;
static int m_mode = APP_ESB_MODE_PTX;
static uint16_t m_current_tx_msg_id;
static uint32_t m_msgq_full_last_time, now;
static size_t pump_calls, write_calls, start_calls, flush_calls, starts;
static size_t producer_pressure, lower_pressure, work_schedules, driver_busy_checks;
static int write_error, start_error;
static bool radio_busy, hardware_has_payload;
static struct esb_payload hardware_payload;
static uint16_t sent_ids[TEST_LOWER_CAPACITY + TEST_PRODUCER_CAPACITY + 8];

static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    CHECK(lock->depth == 0); lock->depth++; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    (void)key; CHECK(lock->depth == 1); lock->depth--;
}
static void k_mutex_lock(int *mutex, int timeout) {
    CHECK(mutex == &event_mutex && timeout == K_FOREVER); (*mutex)++;
}
static void k_mutex_unlock(int *mutex) { CHECK(*mutex > 0); (*mutex)--; }
static uint32_t k_uptime_get_32(void) { return now; }
static void totem_esb_diag_tx_step(int step, int result) { (void)step; (void)result; }
static void totem_esb_transport_queue_pressure(bool producer) {
    if (producer) producer_pressure++; else lower_pressure++;
}
static void add_retry_entry(uint16_t id, uint8_t retries) { (void)id; (void)retries; }
static size_t ring_buf_size_get(const struct ring_buf *ring) { return ring->size; }
static size_t ring_buf_space_get(const struct ring_buf *ring) { return sizeof(ring->bytes) - ring->size; }
static size_t ring_buf_peek(const struct ring_buf *ring, uint8_t *out, size_t size) {
    if (size > ring->size) size = ring->size;
    memcpy(out, ring->bytes, size); return size;
}
static void ring_buf_reset(struct ring_buf *ring) { ring->size = ring->claimed = 0; }
static size_t ring_buf_get_claim(struct ring_buf *ring, uint8_t **out, size_t size) {
    CHECK(tx_ring_lock.depth == 1);
    size_t available = ring->size - ring->claimed;
    if (size > available) size = available;
    *out = ring->bytes + ring->claimed; ring->claimed += size; return size;
}
static void ring_buf_get_finish(struct ring_buf *ring, size_t consumed) {
    CHECK(consumed <= ring->claimed);
    memmove(ring->bytes, ring->bytes + consumed, ring->size - consumed);
    ring->size -= consumed; ring->claimed = 0;
}
static void ring_buf_put_frame(struct ring_buf *ring, const uint8_t *bytes, size_t size) {
    CHECK(tx_ring_lock.depth == 1 && size <= ring_buf_space_get(ring));
    memcpy(ring->bytes + ring->size, bytes, size); ring->size += size;
}
static int k_msgq_put(void *queue, const struct queued_payload *packet, int timeout) {
    CHECK(queue == &m_msgq_tx_payloads && timeout == K_NO_WAIT && m_tx_lock.depth == 1);
    if (m_msgq_tx_payloads.size == TEST_LOWER_CAPACITY) return -ENOMSG;
    m_msgq_tx_payloads.items[m_msgq_tx_payloads.size++] = *packet; return 0;
}
static int k_msgq_peek(void *queue, struct queued_payload *packet) {
    CHECK(queue == &m_msgq_tx_payloads && m_tx_lock.depth == 1);
    if (!m_msgq_tx_payloads.size) return -ENOMSG;
    *packet = m_msgq_tx_payloads.items[0]; return 0;
}
static int k_msgq_get(void *queue, struct queued_payload *packet, int timeout) {
    CHECK(timeout == K_NO_WAIT);
    if (k_msgq_peek(queue, packet)) return -ENOMSG;
    m_msgq_tx_payloads.size--;
    memmove(m_msgq_tx_payloads.items, m_msgq_tx_payloads.items + 1,
            m_msgq_tx_payloads.size * sizeof(*packet)); return 0;
}
static size_t k_msgq_num_used_get(void *queue) {
    CHECK(queue == &m_msgq_tx_payloads); return m_msgq_tx_payloads.size;
}
static int m_hf_work;
static void k_work_reschedule(int *work, int delay) {
    CHECK(work == &m_hf_work && delay == K_NO_WAIT); work_schedules++;
}
static bool esb_is_idle(void) { driver_busy_checks++; return !radio_busy; }
static int service_prx_unlocked(void) { CHECK(false); return -ENOTSUP; }
static int esb_write_payload(const struct esb_payload *payload) {
    CHECK(m_tx_lock.depth == 1 && !radio_busy); write_calls++;
    if (write_error) return write_error;
    CHECK(!hardware_has_payload);
    hardware_payload = *payload; hardware_has_payload = true; return 0;
}
static int esb_start_tx(void) {
    CHECK(m_tx_lock.depth == 1 && hardware_has_payload && !radio_busy); start_calls++;
    if (start_error) return start_error;
    CHECK(starts < sizeof(sent_ids) / sizeof(sent_ids[0]));
    uint16_t id;
    memcpy(&id, hardware_payload.data + sizeof(struct esb_msg_prefix), sizeof(id));
    CHECK(id == m_current_tx_msg_id);
    sent_ids[starts++] = id; radio_busy = true; return 0;
}
static void esb_flush_tx(void) {
    CHECK(m_tx_lock.depth == 1 && !radio_busy); flush_calls++; hardware_has_payload = false;
}

static int zmk_split_esb_send(app_esb_data_t *tx_packet);
/* ACTUAL_PTX_PULL */
/* ACTUAL_PTX_SEND */
/* ACTUAL_COMMON_TX */
/* ACTUAL_BEGIN_TX */

static void make_frame(uint16_t id, uint8_t frame[FRAME_BYTES]) {
    memset(frame, 0xa5, FRAME_BYTES);
    struct esb_msg_prefix prefix = {.magic_prefix = {'Z','m','K','3'}, .payload_size = 8};
    struct esb_msg_meta meta = {.msg_id = id, .max_retry = 3, .pipe = 1};
    memcpy(frame, &prefix, sizeof(prefix));
    memcpy(frame + sizeof(prefix), &id, sizeof(id));
    memcpy(frame + RADIO_BYTES, &meta, sizeof(meta));
}
/* Sealing/sequence assignment is outside this fixture. The exact production
 * admission guard runs against already sealed frames, with its original locks.
 */
static int admit_sealed_frame(const uint8_t *frame, size_t frame_size) {
    k_mutex_lock(&event_mutex, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&tx_ring_lock);
    /* ACTUAL_PRODUCER_ADMISSION */
    ring_buf_put_frame(&tx_buf, frame, frame_size);
    begin_tx();
    k_spin_unlock(&tx_ring_lock, key);
    k_mutex_unlock(&event_mutex);
    return 0;
}
static int admit_id(uint16_t id) {
    uint8_t frame[FRAME_BYTES]; make_frame(id, frame);
    return admit_sealed_frame(frame, sizeof(frame));
}
static void reset_fixture(const char *name) {
    scenario = name;
    memset(&tx_buf, 0, sizeof(tx_buf));
    memset(&m_msgq_tx_payloads, 0, sizeof(m_msgq_tx_payloads));
    memset(sent_ids, 0, sizeof(sent_ids));
    m_current_tx_msg_id = 0; m_msgq_full_last_time = 0; now = 1;
    pump_calls = write_calls = start_calls = flush_calls = starts = 0;
    producer_pressure = lower_pressure = work_schedules = driver_busy_checks = 0;
    write_error = start_error = 0;
    radio_busy = hardware_has_payload = false;
    CHECK(event_mutex == 0 && tx_ring_lock.depth == 0 && m_tx_lock.depth == 0);
}
static void fill_both_queues(void) {
    for (uint16_t id = 1; id <= TEST_LOWER_CAPACITY + TEST_PRODUCER_CAPACITY; id++) {
        CHECK(admit_id(id) == 0);
    }
    CHECK(m_msgq_tx_payloads.size == TEST_LOWER_CAPACITY);
    CHECK(tx_buf.size == sizeof(tx_buf.bytes) && starts == 0);
}
/* Model a completed IRQ only after a successful start. Serialize common TX
 * against producer claims as irq_lock does on the firmware's single core.
 */
static void complete_radio(void) {
    CHECK(radio_busy && hardware_has_payload);
    radio_busy = hardware_has_payload = false; m_current_tx_msg_id = 0;
    k_spinlock_key_t ring_key = k_spin_lock(&tx_ring_lock);
    k_spinlock_key_t key = k_spin_lock(&m_tx_lock);
    (void)pull_packet_from_tx_msgq_unlocked();
    k_spin_unlock(&m_tx_lock, key);
    zmk_split_esb_tx(&state);
    k_spin_unlock(&tx_ring_lock, ring_key);
}
static void full_queues_recover(bool fail_start) {
    reset_fixture(fail_start ? "full_queue_start_failure" : "full_queue_write_failure");
    if (fail_start) start_error = -EBUSY; else write_error = -ENOMEM;
    fill_both_queues();
    write_error = start_error = 0;
    size_t previous_pumps = pump_calls, previous_flushes = flush_calls;
    uint8_t preserved[sizeof(tx_buf.bytes)]; memcpy(preserved, tx_buf.bytes, sizeof(preserved));
    CHECK(admit_id(999) == -ENOSPC);
    CHECK(starts == 1); /* Without the candidate, this proves the permanent stall. */
    CHECK(pump_calls == previous_pumps + 1 && sent_ids[0] == 1);
    CHECK(tx_buf.size == sizeof(tx_buf.bytes) && tx_buf.claimed == 0);
    CHECK(memcmp(tx_buf.bytes, preserved, sizeof(preserved)) == 0);
    CHECK(flush_calls == previous_flushes && work_schedules == 0);
    for (size_t i = 0; i < TEST_LOWER_CAPACITY + TEST_PRODUCER_CAPACITY; i++) {
        complete_radio();
    }
    CHECK(!radio_busy && !hardware_has_payload && tx_buf.size == 0 && m_msgq_tx_payloads.size == 0);
    CHECK(starts == TEST_LOWER_CAPACITY + TEST_PRODUCER_CAPACITY);
    for (size_t i = 0; i < starts; i++) CHECK(sent_ids[i] == i + 1);
}
static void ongoing_failures_and_busy_are_bounded(void) {
    reset_fixture("persistent_write_failure_is_bounded");
    write_error = -ENOMEM; fill_both_queues();
    uint8_t preserved[sizeof(tx_buf.bytes)]; memcpy(preserved, tx_buf.bytes, sizeof(preserved));
    for (unsigned int i = 0; i < 20; i++) {
        size_t previous_pumps = pump_calls, previous_writes = write_calls;
        now += 10;
        CHECK(admit_id(999) == -ENOSPC);
        CHECK(pump_calls == previous_pumps + 1 && write_calls == previous_writes + 2);
        CHECK(starts == 0 && work_schedules == 0);
        CHECK(tx_buf.size == sizeof(tx_buf.bytes) && tx_buf.claimed == 0);
        CHECK(memcmp(tx_buf.bytes, preserved, sizeof(preserved)) == 0);
    }
    scenario = "busy_radio_is_bounded";
    write_error = 0; radio_busy = true;
    for (unsigned int i = 0; i < 20; i++) {
        size_t previous_pumps = pump_calls, previous_writes = write_calls;
        size_t previous_busy = driver_busy_checks;
        now += 10;
        CHECK(admit_id(999) == -ENOSPC);
        CHECK(pump_calls == previous_pumps + 1 && driver_busy_checks == previous_busy + 1);
        CHECK(write_calls == previous_writes && starts == 0 && work_schedules == 0);
        CHECK(tx_buf.claimed == 0 && memcmp(tx_buf.bytes, preserved, sizeof(preserved)) == 0);
    }
    reset_fixture("persistent_start_failure_is_bounded");
    start_error = -EBUSY; fill_both_queues();
    memcpy(preserved, tx_buf.bytes, sizeof(preserved));
    for (unsigned int i = 0; i < 20; i++) {
        size_t previous_pumps = pump_calls, previous_writes = write_calls;
        size_t previous_starts = start_calls, previous_flushes = flush_calls;
        now += 10;
        CHECK(admit_id(999) == -ENOSPC);
        CHECK(pump_calls == previous_pumps + 1 && write_calls == previous_writes + 1);
        CHECK(start_calls == previous_starts + 1 && flush_calls == previous_flushes + 1);
        CHECK(starts == 0 && work_schedules == 0 && !hardware_has_payload);
        CHECK(tx_buf.size == sizeof(tx_buf.bytes) && tx_buf.claimed == 0);
        CHECK(memcmp(tx_buf.bytes, preserved, sizeof(preserved)) == 0);
    }
}
int main(void) {
    full_queues_recover(false);
    full_queues_recover(true);
    ongoing_failures_and_busy_are_bounded();
    puts("Actual TX liveness: write/start recovery, FIFO preservation and bounded pressure passed");
    return 0;
}
