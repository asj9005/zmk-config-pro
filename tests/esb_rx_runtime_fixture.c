/* SPDX-License-Identifier: MIT
 * Ring/driver fakes for the actual ESB receive admission and IRQ dispatch.
 * This checks storage, byte ownership and worker routing, not radio timing.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_ESB_MAX_PAYLOAD_LENGTH 64
#define CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT 2
#define CONFIG_ZMK_SPLIT_ESB_EVENT_BUFFER_ITEMS 128
#define CONFIG_ZMK_SPLIT_ESB_CMD_BUFFER_ITEMS 16
#define RX_BUFFER_SIZE 48
#define ARG_UNUSED(value) (void)(value)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define TOTEM_DIAG_TX_OK 0
#define TOTEM_DIAG_TX_FAIL 1
#define TOTEM_DIAG_RX 2
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); exit(1); \
} } while (0)

struct k_work { int unused; };
struct ring_buf {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t size;
};
static void ring_buf_init(struct ring_buf *ring, uint32_t capacity, uint8_t *data) {
    *ring = (struct ring_buf){.buffer = data, .capacity = capacity};
}
static uint32_t ring_buf_capacity_get(const struct ring_buf *ring) {
    return ring->capacity;
}
static uint32_t ring_buf_space_get(const struct ring_buf *ring) {
    CHECK(ring->buffer != NULL);
    return ring->capacity - ring->size;
}
static bool ring_buf_is_empty(const struct ring_buf *ring) { return ring->size == 0; }
static uint32_t ring_buf_put(struct ring_buf *ring, const uint8_t *data, uint32_t size) {
    CHECK(ring->buffer != NULL);
    CHECK(size <= ring_buf_space_get(ring));
    memcpy(ring->buffer + ring->size, data, size);
    ring->size += size;
    return size;
}

/* ACTUAL_APP_EVENT_TYPES */
typedef void (*zmk_split_esb_process_rx_callback_t)(uint8_t pipe);
/* ACTUAL_TRANSPORT_STATE */
/* ACTUAL_RX_STORAGE */
/* ACTUAL_RX_INIT */

static struct ring_buf tx_buf;
static unsigned int scheduled[CONFIG_ESB_PIPE_COUNT];
static unsigned int invalid_count;
static unsigned int overflow_count;
static unsigned int worker_mask;
static void process_rx_cb(uint8_t pipe) { scheduled[pipe]++; }
static struct zmk_split_esb_state state = {
    .tx_buf = &tx_buf, .rx_bufs = rx_bufs, .process_rx_callback = process_rx_cb,
};
static void zmk_split_esb_tx(struct zmk_split_esb_state *value) { ARG_UNUSED(value); }
static void totem_esb_diag_event(int event, int result) {
    ARG_UNUSED(event); ARG_UNUSED(result);
}
static void totem_esb_benchmark_tx(uint8_t source, uint16_t message,
                                   uint16_t attempts, bool success) {
    ARG_UNUSED(source); ARG_UNUSED(message); ARG_UNUSED(attempts); ARG_UNUSED(success);
}
static void totem_esb_benchmark_rx_invalid(uint8_t pipe, int result) {
    ARG_UNUSED(pipe); CHECK(result == -EADDRNOTAVAIL); invalid_count++;
}
static void totem_esb_benchmark_rx_overflow(uint8_t pipe, uint32_t count) {
    ARG_UNUSED(pipe); ARG_UNUSED(count); overflow_count++;
}
/* ACTUAL_COMMON_CALLBACK */
/* ACTUAL_WORKER_ROUTING */

struct esb_payload {
    uint8_t pipe;
    uint16_t length;
    uint8_t data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
};
static struct esb_payload radio_packets[2];
static unsigned int radio_index;
static uint8_t *driver_payload_data;
static int esb_read_rx_payload(struct esb_payload *payload) {
    driver_payload_data = payload->data;
    if (radio_index == 2) {
        /* Destroy the driver's reused local buffer before the IRQ returns. */
        memset(payload, 0xa5, sizeof(*payload));
        return -ENODATA;
    }
    *payload = radio_packets[radio_index++];
    return 0;
}
static void m_callback(app_esb_event_t *event) {
    CHECK(event->buf == driver_payload_data);
    zmk_split_esb_cb(event, &state);
}
/* ACTUAL_RADIO_RX_DISPATCH */

static bool expected_pipe(uint8_t pipe) {
#if TEST_CENTRAL
    return pipe > 0 && pipe <= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT;
#else
    return pipe == CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID;
#endif
}

int main(void) {
    init_rx_buffers();
    unsigned int expected_mask = 0;
    unsigned int active_pipes = 0;
    uint8_t first_pipe = 0;
    for (uint8_t pipe = 0; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        if (expected_pipe(pipe)) {
            CHECK(rx_bufs[pipe].capacity == RX_RING_BUF_SIZE);
            CHECK(rx_bufs[pipe].buffer != NULL);
            expected_mask |= 1U << pipe;
            active_pipes++;
            if (first_pipe == 0) { first_pipe = pipe; }
        } else {
            CHECK(rx_bufs[pipe].capacity == 0);
            CHECK(rx_bufs[pipe].buffer == NULL);
        }
    }
    CHECK(sizeof(rx_bufs_data) == active_pipes * RX_RING_BUF_SIZE);
    struct k_work work = {0};
    process_rx_work_cb(&work);
    CHECK(worker_mask == expected_mask);

    uint8_t packet[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    for (size_t i = 0; i < sizeof(packet); i++) { packet[i] = (uint8_t)i; }
    /* Include the first out-of-range index and UINT8_MAX before array access. */
    for (unsigned int pipe = 0; pipe <= CONFIG_ESB_PIPE_COUNT; pipe++) {
        app_esb_event_t event = {.evt_type = APP_ESB_EVT_RX, .pipe = (uint8_t)pipe,
                                .buf = packet, .data_length = sizeof(packet)};
        unsigned int before = invalid_count;
        zmk_split_esb_cb(&event, &state);
        if (expected_pipe((uint8_t)pipe)) {
            CHECK(invalid_count == before);
            CHECK(scheduled[pipe] == 1);
            CHECK(rx_bufs[pipe].size == sizeof(packet));
            CHECK(memcmp(rx_bufs[pipe].buffer, packet, sizeof(packet)) == 0);
        } else {
            CHECK(invalid_count == before + 1);
            if (pipe < CONFIG_ESB_PIPE_COUNT) { CHECK(scheduled[pipe] == 0); }
        }
    }
    app_esb_event_t invalid = {.evt_type = APP_ESB_EVT_RX, .pipe = UINT8_MAX,
                              .buf = packet, .data_length = sizeof(packet)};
    unsigned int before = invalid_count;
    zmk_split_esb_cb(&invalid, &state);
    CHECK(invalid_count == before + 1);

    /* Existing capacity remains usable through an exact final byte. */
    for (uint8_t pipe = 1; pipe < CONFIG_ESB_PIPE_COUNT; pipe++) {
        if (!expected_pipe(pipe)) { continue; }
        while (rx_bufs[pipe].size < RX_RING_BUF_SIZE) {
            uint32_t remaining = RX_RING_BUF_SIZE - rx_bufs[pipe].size;
            app_esb_event_t event = {.evt_type = APP_ESB_EVT_RX, .pipe = pipe,
                .buf = packet, .data_length = remaining < sizeof(packet) ? remaining : sizeof(packet)};
            zmk_split_esb_cb(&event, &state);
        }
        unsigned int scheduled_before = scheduled[pipe];
        unsigned int overflow_before = overflow_count;
        app_esb_event_t extra = {.evt_type = APP_ESB_EVT_RX, .pipe = pipe,
                                .buf = packet, .data_length = 1};
        zmk_split_esb_cb(&extra, &state);
        CHECK(rx_bufs[pipe].size == RX_RING_BUF_SIZE);
        CHECK(scheduled[pipe] == scheduled_before);
        CHECK(overflow_count == overflow_before + 1);
        rx_bufs[pipe].size = 0;
    }
    radio_packets[0] = (struct esb_payload){.pipe = first_pipe, .length = 64};
    radio_packets[1] = (struct esb_payload){.pipe = first_pipe, .length = 17};
    memset(radio_packets[0].data, 0x31, 64);
    memset(radio_packets[1].data, 0x72, 17);
    receive_from_radio();
    CHECK(rx_bufs[first_pipe].size == 81);
    CHECK(memcmp(rx_bufs[first_pipe].buffer, radio_packets[0].data, 64) == 0);
    CHECK(memcmp(rx_bufs[first_pipe].buffer + 64, radio_packets[1].data, 17) == 0);
    puts("ESB RX storage, routing, boundary and buffer lifetime checks passed");
    return 0;
}
