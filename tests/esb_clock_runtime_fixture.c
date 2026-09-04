/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>

#define ARG_UNUSED(x) ((void)(x))
#define LOG_ERR(...) ((void)0)
#define K_MSEC(ms) (ms)
#define K_NO_WAIT 0
#define CONFIG_TOTEM_ESB_HF_IDLE_MS 10
#define ONOFF_FLAG_ERROR 1
#define APP_ESB_MODE_PTX 0
#define APP_ESB_MODE_PRX 1

struct k_work { int unused; };
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
typedef int atomic_t;
struct onoff_manager { int unused; };
struct onoff_client;
typedef void (*ready_fn)(struct onoff_manager *, struct onoff_client *, uint32_t, int);
struct onoff_client { struct { ready_fn callback; } notify; };

static struct k_spinlock m_tx_lock;
static struct k_work m_hf_work;
static struct onoff_manager manager;
static struct onoff_manager *m_hf_manager = &manager;
static struct onoff_client m_hf_client;
static bool m_hf_ready, m_hf_requested, m_hf_pending;
static bool m_active, m_enabled, radio_idle;
static unsigned int m_current_tx_msg_id;
static int m_mode, m_msgq_tx_payloads;
static uint32_t m_hf_idle_since, now;
static atomic_t m_hf_result;
static unsigned int requests, releases, transmissions, lock_depth;
static int scheduled_delay, request_error;
static bool synchronous_completion, enqueue_during_release;

static int atomic_get(atomic_t *value) { return *value; }
static void atomic_set(atomic_t *value, int next) { *value = next; }
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    ARG_UNUSED(lock);
    assert(lock_depth++ == 0);
    return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    ARG_UNUSED(lock); ARG_UNUSED(key);
    assert(--lock_depth == 0);
}
static void k_work_reschedule(struct k_work *work, int delay) {
    ARG_UNUSED(work); scheduled_delay = delay;
}
static unsigned int k_msgq_num_used_get(int *queue) { return *queue; }
static bool esb_is_idle(void) { return radio_idle; }
static uint32_t k_uptime_get_32(void) { return now; }
static int pull_packet_from_tx_msgq_unlocked(void) {
    assert(lock_depth == 1 && m_hf_ready && m_enabled && m_active);
    if (m_msgq_tx_payloads > 0 && radio_idle) {
        m_msgq_tx_payloads--;
        radio_idle = false;
        m_current_tx_msg_id = ++transmissions;
    }
    return 0;
}
static int onoff_release(struct onoff_manager *mgr) {
    assert(mgr == m_hf_manager && lock_depth == 0 && !m_hf_ready);
    releases++;
    if (enqueue_during_release) {
        m_msgq_tx_payloads++;
        k_work_reschedule(&m_hf_work, K_NO_WAIT);
    }
    return 0;
}
static void sys_notify_init_callback(void *notify, ready_fn callback) {
    ARG_UNUSED(notify); m_hf_client.notify.callback = callback;
}
static int onoff_request(struct onoff_manager *mgr, struct onoff_client *client) {
    assert(mgr == m_hf_manager && lock_depth == 0 && m_hf_pending);
    requests++;
    if (request_error != 0) {
        return request_error;
    }
    if (synchronous_completion) {
        client->notify.callback(mgr, client, 0, 0);
    }
    return 0;
}

/* ACTUAL_FIRMWARE_FUNCTIONS */

static void reset(void) {
    m_mode = APP_ESB_MODE_PTX;
    m_hf_ready = m_hf_requested = true;
    m_hf_pending = false;
    m_hf_result = INT_MIN;
    m_active = m_enabled = radio_idle = true;
    m_current_tx_msg_id = 0;
    m_msgq_tx_payloads = 0;
    m_hf_idle_since = 0;
    now = 10;
    requests = releases = transmissions = lock_depth = 0;
    scheduled_delay = -1;
    request_error = 0;
    synchronous_completion = enqueue_during_release = false;
}

int main(void) {
    reset();
    hf_clock_work_handler(&m_hf_work);
    assert(releases == 1 && !m_hf_ready && !m_hf_requested);
    m_msgq_tx_payloads = 1;
    hf_clock_work_handler(&m_hf_work);
    assert(requests == 1 && m_hf_pending && transmissions == 0);
    hf_clock_work_handler(&m_hf_work);
    assert(requests == 1 && transmissions == 0); /* asynchronous wait */
    hf_clock_ready(m_hf_manager, &m_hf_client, 0, 0);
    assert(transmissions == 0 && scheduled_delay == 0); /* IRQ only schedules */
    hf_clock_work_handler(&m_hf_work);
    assert(transmissions == 1 && m_hf_ready && !m_hf_pending);
    hf_clock_work_handler(&m_hf_work);
    assert(releases == 1); /* RF transaction still running */
    radio_idle = true;
    hf_clock_work_handler(&m_hf_work);
    assert(releases == 1); /* completion callback has not consumed TX ID yet */
    m_current_tx_msg_id = 0;
    m_hf_idle_since = now;
    hf_clock_work_handler(&m_hf_work);
    assert(releases == 1 && scheduled_delay == 10);
    m_msgq_tx_payloads = 1;
    hf_clock_work_handler(&m_hf_work);
    assert(transmissions == 2 && requests == 1); /* next burst keeps HFXO */

    reset();
    enqueue_during_release = true;
    hf_clock_work_handler(&m_hf_work);
    assert(releases == 1 && m_msgq_tx_payloads == 1 && !m_hf_ready);
    synchronous_completion = true;
    hf_clock_work_handler(&m_hf_work);
    assert(requests == 1 && transmissions == 0 && scheduled_delay == 0);
    hf_clock_work_handler(&m_hf_work);
    assert(transmissions == 1); /* inline onoff completion does not deadlock */

    reset();
    hf_clock_work_handler(&m_hf_work);
    m_msgq_tx_payloads = 1;
    hf_clock_work_handler(&m_hf_work);
    m_active = m_enabled = false;
    hf_clock_ready(m_hf_manager, &m_hf_client, 0, 0);
    hf_clock_work_handler(&m_hf_work);
    assert(transmissions == 0 && releases == 2 && !m_hf_ready);

    reset();
    hf_clock_work_handler(&m_hf_work);
    m_msgq_tx_payloads = 1;
    request_error = -EIO;
    hf_clock_work_handler(&m_hf_work);
    assert(requests == 1 && transmissions == 0 && scheduled_delay == 100);
    hf_clock_work_handler(&m_hf_work);
    assert(!m_hf_pending && !m_hf_ready && scheduled_delay == 100);
    request_error = 0;
    synchronous_completion = true;
    hf_clock_work_handler(&m_hf_work);
    hf_clock_work_handler(&m_hf_work);
    assert(transmissions == 1 && requests == 2);

    reset();
    m_mode = APP_ESB_MODE_PRX;
    hf_clock_work_handler(&m_hf_work);
    assert(requests == 0 && releases == 0 && m_hf_ready);
    puts("Actual HFCLK work/callback: async, retry, in-flight and release-race cases passed");
    return 0;
}
