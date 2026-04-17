/*
 * log_service.c — AZSensorSuite firmware, sensor log and BLE bulk stream
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file log_service.c
 * @brief Detection log storage and BLE bulk download stream.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/fs/zms.h>
#include <zephyr/storage/flash_map.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/spinlock.h>

#include <errno.h>
#include <string.h>

#include "log_service.h"
#include "sense_service.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(log_svc, LOG_LEVEL_INF);

/* Provided by main.c */
extern struct bt_conn  *conn;
extern struct k_mutex   conn_mutex;

/* Defined by BT_GATT_SERVICE_DEFINE in sense_service.c */
extern struct bt_gatt_service sense_service_svc;

/* ════════════════════════════════════════════════════════════════
 * Definitions
 * ════════════════════════════════════════════════════════════════ */

/* ZMS storage */
#define STAMP_ENTRY_BYTES         (ZMS_STAMP_CHUNK_SIZE   * sizeof(int32_t))
#define AMBIENT_ENTRY_BYTES       (ZMS_AMBIENT_CHUNK_SIZE * sizeof(struct ambient_sample))
#define ZMS_STAGE_BUF_SIZE        MAX(STAMP_ENTRY_BYTES, AMBIENT_ENTRY_BYTES)
#define ZMS_MAX_EVICTION_ATTEMPTS CONFIG_APP_ZMS_MAX_EVICTION_ATTEMPTS
#define ZMS_META_KEY              0U
#define ZMS_DATA_KEY_START        1U
/* ZMS_PERSIST_KEY is a reserved high key for the power-loss state record */
#define ZMS_PERSIST_KEY           0xFFFF0000U
#define PERSIST_MAGIC             0x415A5353U /* "AZSS" */

/* BLE bulk stream */
/* BULK_ATTR_INDEX is the BULK characteristic index in sense_service_svc.attrs */
#define BULK_ATTR_INDEX 11
#define STREAM_MAX_ERR  CONFIG_APP_STREAM_MAX_ERR

/* ════════════════════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════════════════════ */

struct ambient_sample {
    int8_t  temp_c;    /* °C, clamped to int8_t range */
    uint8_t humid_pct; /* %RH, 0–100                  */
};

struct zms_log_meta {
    uint32_t write_key;   /* next key to write into                */
    uint32_t oldest_key;  /* oldest valid data key (for streaming) */
    uint32_t entry_count; /* entries currently stored              */
};

struct log_persist_record {
    uint32_t magic;
    int64_t  last_event_unix_ms;
    int64_t  carry_ms;
    int64_t  current_time_ms;
};

/* 4-phase download: ZMS stamps → RAM stamp tail → ZMS ambient → RAM ambient tail */
enum stream_phase {
    PHASE_ZMS_STAMP   = 0,
    PHASE_RAM_STAMP   = 1,
    PHASE_ZMS_AMBIENT = 2,
    PHASE_RAM_AMBIENT = 3,
    PHASE_DONE        = 4,
};

/* ════════════════════════════════════════════════════════════════
 * Forward declarations
 * ════════════════════════════════════════════════════════════════ */

static void stamp_commit_work_handler(struct k_work *work);
static void ambient_commit_work_handler(struct k_work *work);
static void bulk_stream_work_handler(struct k_work *work);

/* ════════════════════════════════════════════════════════════════
 * State
 * ════════════════════════════════════════════════════════════════ */

/* RAM ring buffers — fast ingestion path, protected by spinlock */

RING_BUF_DECLARE(rb_stamp,   RB_STAMP_SIZE);
RING_BUF_DECLARE(rb_ambient, RB_AMBIENT_SIZE);

static struct k_spinlock rb_stamp_lock;
static struct k_spinlock rb_ambient_lock;

/* ZMS */

static struct zms_fs       stamp_zms;
static struct zms_fs       ambient_zms;
static struct zms_log_meta stamp_meta;
static struct zms_log_meta ambient_meta;

K_MUTEX_DEFINE(stamp_zms_mtx);
K_MUTEX_DEFINE(ambient_zms_mtx);

/* Atomic mirrors of entry_count — allow lock-free reads from BT thread */
static atomic_t zms_stamp_entry_count_a   = ATOMIC_INIT(0);
static atomic_t zms_ambient_entry_count_a = ATOMIC_INIT(0);
static atomic_t zms_ready                 = ATOMIC_INIT(0);

/* BLE bulk stream */

static uint8_t bulk_pkt[BULK_PKT_MAX];

static struct k_work_delayable      bulk_stream_work;
static struct bt_gatt_notify_params bulk_ntf_params;

static atomic_t          bulk_stream_active;
static atomic_t          bulk_in_flight;
static uint16_t          bulk_seq;
static int64_t           stream_last_sent_ms;
static uint8_t           stream_err_count;
static enum stream_phase cur_phase;

K_THREAD_STACK_DEFINE(stream_wq_stack, CONFIG_APP_STREAM_WQ_STACK_SIZE);
static struct k_work_q stream_wq;

static K_WORK_DEFINE(stamp_commit_work,   stamp_commit_work_handler);
static K_WORK_DEFINE(ambient_commit_work, ambient_commit_work_handler);

/* ZMS read cursors — snapshotted at stream start so new commits during
 * a download cannot shift the iteration boundary */
static uint32_t zms_rkey_stamp;
static uint32_t zms_end_key_stamp;
static uint32_t zms_rkey_ambient;
static uint32_t zms_end_key_ambient;

/* One-chunk staging buffer shared across ZMS phases */
static uint8_t  zms_stage[ZMS_STAGE_BUF_SIZE];
static uint16_t zms_stage_len;
static uint16_t zms_stage_offset;

/* ════════════════════════════════════════════════════════════════
 * Public query
 * ════════════════════════════════════════════════════════════════ */

bool bulk_stream_is_active(void)
{
    return atomic_get(&bulk_stream_active);
}

/* ════════════════════════════════════════════════════════════════
 * ZMS helpers
 * ════════════════════════════════════════════════════════════════ */

/* Caller must hold the appropriate ZMS mutex */
static int save_meta(struct zms_fs *fs, const struct zms_log_meta *m)
{
    ssize_t rc = zms_write(fs, ZMS_META_KEY, m, sizeof(*m));
    if (rc != (ssize_t)sizeof(*m)) {
        LOG_ERR("meta write failed (rc=%d)", (int)rc);
        return (rc < 0) ? (int)rc : -EIO;
    }
    return 0;
}

/* Write one chunk to ZMS. Caller must hold the appropriate ZMS mutex.
 * In OVERWRITE_OLDEST mode, evicts old entries up to ZMS_MAX_EVICTION_ATTEMPTS
 * before giving up, preventing an infinite loop on persistent zms_delete failures. */
static int commit_chunk_locked(struct zms_fs *fs,
                               struct zms_log_meta *meta,
                               atomic_t *entry_count_a,
                               const void *data, size_t len)
{
    ssize_t rc = 0;

#if IS_ENABLED(CONFIG_APP_LOG_OVERWRITE_OLDEST)
    for (uint8_t attempt = 0U; attempt < ZMS_MAX_EVICTION_ATTEMPTS; attempt++) {
        rc = zms_write(fs, meta->write_key, data, len);

        if (rc != -ENOSPC) {
            break;
        }

        if (meta->entry_count == 0U) {
            LOG_ERR("ZMS full but entry_count==0 after %u evictions, dropping", attempt);
            return -ENOSPC;
        }

        int del = zms_delete(fs, meta->oldest_key);
        if (del != 0 && del != -ENOENT) {
            LOG_ERR("zms_delete key %u failed: %d", meta->oldest_key, del);
            /* Advance past it regardless — accounting must stay consistent */
        }

        meta->oldest_key++;
        if (meta->oldest_key == ZMS_META_KEY) {
            meta->oldest_key = ZMS_DATA_KEY_START;
        }
        meta->entry_count--;
        atomic_dec(entry_count_a);
    }

    if (rc == -ENOSPC) {
        LOG_ERR("ZMS still full after %u evictions, dropping chunk", ZMS_MAX_EVICTION_ATTEMPTS);
        return -ENOSPC;
    }
#else
    rc = zms_write(fs, meta->write_key, data, len);
    if (rc == -ENOSPC) {
        LOG_WRN("ZMS full (STOP_WHEN_FULL), dropping chunk");
        return -ENOSPC;
    }
#endif

    if (rc < 0) {
        LOG_ERR("zms_write key %u failed: %d", meta->write_key, (int)rc);
        return (int)rc;
    }

    meta->write_key++;
    if (meta->write_key == ZMS_META_KEY) {
        meta->write_key = ZMS_DATA_KEY_START;
    }
    meta->entry_count++;
    atomic_inc(entry_count_a);

    /* Best-effort: if power is lost between data write and meta write, the
     * orphaned entry occupies RRAM but is never iterated or evicted — an
     * accepted low-probability leak of one chunk per power cycle during a write. */
    (void)save_meta(fs, meta);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * Background commit handlers
 * ════════════════════════════════════════════════════════════════ */

static void stamp_commit_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Never commit during a stream — RAM tail is drained by PHASE_RAM_STAMP.
     * Commit work is cancelled synchronously before stream start; this is
     * a belt-and-braces guard only. */
    if (atomic_get(&bulk_stream_active)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&rb_stamp_lock);
    uint8_t *ptr     = NULL;
    uint32_t claimed = ring_buf_get_claim(&rb_stamp, &ptr, STAMP_ENTRY_BYTES);
    k_spin_unlock(&rb_stamp_lock, key);

    if (claimed < STAMP_ENTRY_BYTES) {
        key = k_spin_lock(&rb_stamp_lock);
        (void)ring_buf_get_finish(&rb_stamp, 0U);
        k_spin_unlock(&rb_stamp_lock, key);
        return;
    }

    /* ZMS write outside the spinlock — RRAM is fast but the spinlock
     * must not be held across a potentially blocking operation */
    k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
    int err = commit_chunk_locked(&stamp_zms, &stamp_meta,
                                  &zms_stamp_entry_count_a,
                                  ptr, STAMP_ENTRY_BYTES);
    k_mutex_unlock(&stamp_zms_mtx);

    key = k_spin_lock(&rb_stamp_lock);
    (void)ring_buf_get_finish(&rb_stamp, (err == 0) ? STAMP_ENTRY_BYTES : 0U);
    k_spin_unlock(&rb_stamp_lock, key);

    if (err == 0) {
        LOG_DBG("stamp chunk committed: %u entries stored", stamp_meta.entry_count);
    }
}

static void ambient_commit_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (atomic_get(&bulk_stream_active)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&rb_ambient_lock);
    uint8_t *ptr     = NULL;
    uint32_t claimed = ring_buf_get_claim(&rb_ambient, &ptr, AMBIENT_ENTRY_BYTES);
    k_spin_unlock(&rb_ambient_lock, key);

    if (claimed < AMBIENT_ENTRY_BYTES) {
        key = k_spin_lock(&rb_ambient_lock);
        (void)ring_buf_get_finish(&rb_ambient, 0U);
        k_spin_unlock(&rb_ambient_lock, key);
        return;
    }

    k_mutex_lock(&ambient_zms_mtx, K_FOREVER);
    int err = commit_chunk_locked(&ambient_zms, &ambient_meta,
                                  &zms_ambient_entry_count_a,
                                  ptr, AMBIENT_ENTRY_BYTES);
    k_mutex_unlock(&ambient_zms_mtx);

    key = k_spin_lock(&rb_ambient_lock);
    (void)ring_buf_get_finish(&rb_ambient, (err == 0) ? AMBIENT_ENTRY_BYTES : 0U);
    k_spin_unlock(&rb_ambient_lock, key);

    if (err == 0) {
        LOG_DBG("ambient chunk committed: %u entries stored", ambient_meta.entry_count);
    }
}

/* ════════════════════════════════════════════════════════════════
 * Ingestion API
 * ════════════════════════════════════════════════════════════════ */

int rb_stamp_put(int32_t value)
{
    k_spinlock_key_t key = k_spin_lock(&rb_stamp_lock);

    if (ring_buf_space_get(&rb_stamp) < sizeof(value)) {
        k_spin_unlock(&rb_stamp_lock, key);
        return -ENOSPC;
    }

    uint32_t w = ring_buf_put(&rb_stamp, (const uint8_t *)&value, sizeof(value));
    bool chunk_ready = (ring_buf_size_get(&rb_stamp) >= STAMP_ENTRY_BYTES);

    k_spin_unlock(&rb_stamp_lock, key);

    if (w != sizeof(value)) {
        return -EIO;
    }

    if (atomic_get(&zms_ready) && chunk_ready) {
        k_work_submit(&stamp_commit_work);
    }
    return 0;
}

int rb_stamp_get(int32_t *value)
{
    if (!value) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&rb_stamp_lock);

    if (ring_buf_size_get(&rb_stamp) < sizeof(*value)) {
        k_spin_unlock(&rb_stamp_lock, key);
        return -ENOENT;
    }

    uint32_t g = ring_buf_get(&rb_stamp, (uint8_t *)value, sizeof(*value));
    k_spin_unlock(&rb_stamp_lock, key);

    return (g == sizeof(*value)) ? 0 : -EIO;
}

int rb_ambient_put(const uint8_t *data, size_t len)
{
    if (!data || len == 0U) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&rb_ambient_lock);

    if (ring_buf_space_get(&rb_ambient) < len) {
        k_spin_unlock(&rb_ambient_lock, key);
        return -ENOSPC;
    }

    uint32_t w = ring_buf_put(&rb_ambient, data, len);
    bool chunk_ready = (ring_buf_size_get(&rb_ambient) >= AMBIENT_ENTRY_BYTES);

    k_spin_unlock(&rb_ambient_lock, key);

    if (w != len) {
        return -EIO;
    }

    if (atomic_get(&zms_ready) && chunk_ready) {
        k_work_submit(&ambient_commit_work);
    }
    return 0;
}

int rb_ambient_get(uint8_t *data, size_t len)
{
    if (!data || len == 0U) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&rb_ambient_lock);

    if (ring_buf_size_get(&rb_ambient) < len) {
        k_spin_unlock(&rb_ambient_lock, key);
        return -ENOENT;
    }

    uint32_t g = ring_buf_get(&rb_ambient, data, len);
    k_spin_unlock(&rb_ambient_lock, key);

    return (g == len) ? 0 : -EIO;
}

uint32_t rb_stamp_count(void)
{
    k_spinlock_key_t key = k_spin_lock(&rb_stamp_lock);
    uint32_t n = ring_buf_size_get(&rb_stamp) / sizeof(int32_t);
    k_spin_unlock(&rb_stamp_lock, key);
    return n;
}

uint32_t log_total_stamp_count(void)
{
    /* Uses atomic mirror — safe to call from BT thread without taking the ZMS mutex */
    uint32_t zms_entries = (uint32_t)atomic_get(&zms_stamp_entry_count_a);
    uint32_t zms_samples = zms_entries * ZMS_STAMP_CHUNK_SIZE;
    uint32_t ram_samples = rb_stamp_count();
    return zms_samples + ram_samples;
}

uint32_t log_total_ambient_count(void)
{
    uint32_t zms_entries = (uint32_t)atomic_get(&zms_ambient_entry_count_a);
    uint32_t zms_samples = zms_entries * ZMS_AMBIENT_CHUNK_SIZE;

    k_spinlock_key_t key = k_spin_lock(&rb_ambient_lock);
    uint32_t ram_samples = ring_buf_size_get(&rb_ambient) / sizeof(struct ambient_sample);
    k_spin_unlock(&rb_ambient_lock, key);

    return zms_samples + ram_samples;
}

/* ════════════════════════════════════════════════════════════════
 * Persist state
 * ════════════════════════════════════════════════════════════════ */

int log_persist_state(int64_t last_event_unix_ms,
                      int64_t carry_ms,
                      int64_t current_time_ms)
{
    if (!atomic_get(&zms_ready)) {
        return 0;
    }

    struct log_persist_record rec = {
        .magic              = PERSIST_MAGIC,
        .last_event_unix_ms = last_event_unix_ms,
        .carry_ms           = carry_ms,
        .current_time_ms    = current_time_ms,
    };

    k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
    ssize_t rc = zms_write(&stamp_zms, ZMS_PERSIST_KEY, &rec, sizeof(rec));
    k_mutex_unlock(&stamp_zms_mtx);

    if (rc != (ssize_t)sizeof(rec)) {
        LOG_ERR("persist_state write failed (rc=%d)", (int)rc);
        return (rc < 0) ? (int)rc : -EIO;
    }

    LOG_DBG("state persisted: last_event=%lld carry=%lld time=%lld",
            (long long)last_event_unix_ms,
            (long long)carry_ms,
            (long long)current_time_ms);
    return 0;
}

bool log_restore_state(int64_t *last_event_unix_ms,
                       int64_t *carry_ms,
                       int64_t *seeded_time_ms)
{
    if (!last_event_unix_ms || !carry_ms || !seeded_time_ms) {
        return false;
    }

    *last_event_unix_ms = 0;
    *carry_ms           = 0;
    *seeded_time_ms     = 0;

    if (!atomic_get(&zms_ready)) {
        return false;
    }

    struct log_persist_record rec;

    k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
    ssize_t rc = zms_read(&stamp_zms, ZMS_PERSIST_KEY, &rec, sizeof(rec));
    k_mutex_unlock(&stamp_zms_mtx);

    if (rc != (ssize_t)sizeof(rec)) {
        LOG_INF("no persist record found (rc=%d) — first boot or cleared", (int)rc);
        return false;
    }

    if (rec.magic != PERSIST_MAGIC) {
        LOG_WRN("persist record magic mismatch (0x%08X) — ignoring", rec.magic);
        return false;
    }

    if (rec.current_time_ms <= 0 ||
        rec.last_event_unix_ms < 0 ||
        rec.carry_ms < 0 ||
        rec.current_time_ms < rec.last_event_unix_ms) {
        LOG_WRN("persist record values out of range — ignoring");
        return false;
    }

    *last_event_unix_ms = rec.last_event_unix_ms;
    *carry_ms           = rec.carry_ms;
    *seeded_time_ms     = rec.current_time_ms;

    LOG_INF("state restored: last_event=%lld carry=%lld seeded_time=%lld",
            (long long)*last_event_unix_ms,
            (long long)*carry_ms,
            (long long)*seeded_time_ms);
    return true;
}

/* ════════════════════════════════════════════════════════════════
 * Service init and clear
 * ════════════════════════════════════════════════════════════════ */

int log_service_init(void)
{
    int err;
    ssize_t rc = 0;

    stamp_zms.flash_device = FIXED_PARTITION_DEVICE(log_stamp_partition);
    stamp_zms.offset       = FIXED_PARTITION_OFFSET(log_stamp_partition);
    stamp_zms.sector_size  = LOG_ZMS_SECTOR_SIZE;
    stamp_zms.sector_count = FIXED_PARTITION_SIZE(log_stamp_partition) / LOG_ZMS_SECTOR_SIZE;

    err = zms_mount(&stamp_zms);
    if (err) {
        LOG_ERR("stamp ZMS mount failed: %d", err);
        return err;
    }

    rc = zms_read(&stamp_zms, ZMS_META_KEY, &stamp_meta, sizeof(stamp_meta));
    if (rc == (ssize_t)sizeof(stamp_meta)) {
        atomic_set(&zms_stamp_entry_count_a, (atomic_val_t)stamp_meta.entry_count);
        LOG_INF("stamp ZMS restored: %u entries (oldest=%u next=%u)",
                stamp_meta.entry_count, stamp_meta.oldest_key, stamp_meta.write_key);
    } else {
        LOG_INF("stamp ZMS: no meta found, starting fresh");
        stamp_meta = (struct zms_log_meta){
            .write_key   = ZMS_DATA_KEY_START,
            .oldest_key  = ZMS_DATA_KEY_START,
            .entry_count = 0U,
        };
        atomic_set(&zms_stamp_entry_count_a, 0);
    }

    ambient_zms.flash_device = FIXED_PARTITION_DEVICE(log_ambient_partition);
    ambient_zms.offset       = FIXED_PARTITION_OFFSET(log_ambient_partition);
    ambient_zms.sector_size  = LOG_ZMS_SECTOR_SIZE;
    ambient_zms.sector_count = FIXED_PARTITION_SIZE(log_ambient_partition) / LOG_ZMS_SECTOR_SIZE;

    err = zms_mount(&ambient_zms);
    if (err) {
        LOG_ERR("ambient ZMS mount failed: %d", err);
        return err;
    }

    rc = zms_read(&ambient_zms, ZMS_META_KEY, &ambient_meta, sizeof(ambient_meta));
    if (rc == (ssize_t)sizeof(ambient_meta)) {
        atomic_set(&zms_ambient_entry_count_a, (atomic_val_t)ambient_meta.entry_count);
        LOG_INF("ambient ZMS restored: %u entries (oldest=%u next=%u)",
                ambient_meta.entry_count, ambient_meta.oldest_key, ambient_meta.write_key);
    } else {
        LOG_INF("ambient ZMS: no meta found, starting fresh");
        ambient_meta = (struct zms_log_meta){
            .write_key   = ZMS_DATA_KEY_START,
            .oldest_key  = ZMS_DATA_KEY_START,
            .entry_count = 0U,
        };
        atomic_set(&zms_ambient_entry_count_a, 0);
    }

    atomic_set(&zms_ready, 1);

    LOG_INF("log service ZMS ready (stamp=%uKB ambient=%uKB strategy=%s)",
            FIXED_PARTITION_SIZE(log_stamp_partition)   / 1024U,
            FIXED_PARTITION_SIZE(log_ambient_partition) / 1024U,
            IS_ENABLED(CONFIG_APP_LOG_OVERWRITE_OLDEST) ? "OVERWRITE_OLDEST"
                                                        : "STOP_WHEN_FULL");
    return 0;
}

void log_service_clear(void)
{
    bulk_stream_stop();

    /* Synchronously cancel commit work before touching ZMS or ring buffers */
    struct k_work_sync sync;
    k_work_cancel_sync(&stamp_commit_work,   &sync);
    k_work_cancel_sync(&ambient_commit_work, &sync);

    if (!atomic_get(&zms_ready)) {
        goto clear_ram;
    }

    k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
    int err = zms_clear(&stamp_zms);
    if (err) {
        LOG_ERR("stamp zms_clear failed: %d", err);
    }
    stamp_meta = (struct zms_log_meta){
        .write_key   = ZMS_DATA_KEY_START,
        .oldest_key  = ZMS_DATA_KEY_START,
        .entry_count = 0U,
    };
    atomic_set(&zms_stamp_entry_count_a, 0);
    /* zms_clear unmounts the filesystem; remount to restore usability */
    err = zms_mount(&stamp_zms);
    if (err) {
        LOG_ERR("stamp ZMS remount failed: %d", err);
    }
    k_mutex_unlock(&stamp_zms_mtx);

    k_mutex_lock(&ambient_zms_mtx, K_FOREVER);
    err = zms_clear(&ambient_zms);
    if (err) {
        LOG_ERR("ambient zms_clear failed: %d", err);
    }
    ambient_meta = (struct zms_log_meta){
        .write_key   = ZMS_DATA_KEY_START,
        .oldest_key  = ZMS_DATA_KEY_START,
        .entry_count = 0U,
    };
    atomic_set(&zms_ambient_entry_count_a, 0);
    err = zms_mount(&ambient_zms);
    if (err) {
        LOG_ERR("ambient ZMS remount failed: %d", err);
    }
    k_mutex_unlock(&ambient_zms_mtx);

clear_ram:;
    k_spinlock_key_t key = k_spin_lock(&rb_stamp_lock);
    ring_buf_reset(&rb_stamp);
    k_spin_unlock(&rb_stamp_lock, key);

    key = k_spin_lock(&rb_ambient_lock);
    ring_buf_reset(&rb_ambient);
    k_spin_unlock(&rb_ambient_lock, key);

    LOG_INF("log cleared");
}

/* ════════════════════════════════════════════════════════════════
 * BLE notification helpers
 * ════════════════════════════════════════════════════════════════ */

static uint16_t max_payload_bytes(struct bt_conn *c)
{
    uint16_t mtu     = bt_gatt_get_mtu(c);
    uint16_t max_ntf = (mtu > 3U) ? (mtu - 3U) : 0U;

    if (max_ntf <= BULK_HDR_LEN) {
        return 0U;
    }

    return MIN((uint16_t)BULK_PKT_MAX, max_ntf) - BULK_HDR_LEN;
}

static void bulk_sent_cb(struct bt_conn *c, void *user_data)
{
    ARG_UNUSED(c);
    ARG_UNUSED(user_data);

    stream_last_sent_ms = k_uptime_get();
    atomic_set(&bulk_in_flight, false);

    if (atomic_get(&bulk_stream_active)) {
        k_work_schedule_for_queue(&stream_wq, &bulk_stream_work, K_NO_WAIT);
    }
}

static int bulk_notify_once(struct bt_conn *c, const uint8_t *data, uint16_t len)
{
    bulk_ntf_params.attr      = &sense_service_svc.attrs[BULK_ATTR_INDEX];
    bulk_ntf_params.data      = data;
    bulk_ntf_params.len       = len;
    bulk_ntf_params.func      = bulk_sent_cb;
    bulk_ntf_params.user_data = NULL;

    return bt_gatt_notify_cb(c, &bulk_ntf_params);
}

static int send_header_only(struct bt_conn *c, uint8_t stream_id, uint8_t flags)
{
    bulk_pkt[0] = stream_id;
    sys_put_le16(bulk_seq, &bulk_pkt[1]);
    bulk_pkt[3] = flags;

    atomic_set(&bulk_in_flight, true);
    int err = bulk_notify_once(c, bulk_pkt, BULK_HDR_LEN);
    if (err == 0) {
        bulk_seq++;
    } else {
        atomic_set(&bulk_in_flight, false);
    }
    return err;
}

/* ════════════════════════════════════════════════════════════════
 * ZMS staging helpers
 * ════════════════════════════════════════════════════════════════ */

/* Read the next stamp chunk into zms_stage. Advances past missing keys —
 * OVERWRITE_OLDEST may have evicted entries since the stream started. */
static bool load_next_zms_stamp(void)
{
    if (!atomic_get(&zms_ready)) {
        return false;
    }

    while (zms_rkey_stamp < zms_end_key_stamp) {
        k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
        ssize_t rc = zms_read(&stamp_zms, zms_rkey_stamp, zms_stage, STAMP_ENTRY_BYTES);
        k_mutex_unlock(&stamp_zms_mtx);

        zms_rkey_stamp++;

        if (rc > 0) {
            zms_stage_len    = (uint16_t)rc;
            zms_stage_offset = 0U;
            return true;
        }
        LOG_DBG("ZMS stamp key %u not found (rc=%d), skipping",
                zms_rkey_stamp - 1U, (int)rc);
    }

    return false;
}

static bool load_next_zms_ambient(void)
{
    if (!atomic_get(&zms_ready)) {
        return false;
    }

    while (zms_rkey_ambient < zms_end_key_ambient) {
        k_mutex_lock(&ambient_zms_mtx, K_FOREVER);
        ssize_t rc = zms_read(&ambient_zms, zms_rkey_ambient, zms_stage, AMBIENT_ENTRY_BYTES);
        k_mutex_unlock(&ambient_zms_mtx);

        zms_rkey_ambient++;

        if (rc > 0) {
            zms_stage_len    = (uint16_t)rc;
            zms_stage_offset = 0U;
            return true;
        }
        LOG_DBG("ZMS ambient key %u not found (rc=%d), skipping",
                zms_rkey_ambient - 1U, (int)rc);
    }

    return false;
}

static int send_from_stage(struct bt_conn *c, uint8_t stream_id, uint16_t cap)
{
    uint16_t avail   = zms_stage_len - zms_stage_offset;
    uint16_t to_send = MIN(avail, cap);

    bulk_pkt[0] = stream_id;
    sys_put_le16(bulk_seq, &bulk_pkt[1]);
    bulk_pkt[3] = 0U;
    memcpy(&bulk_pkt[BULK_HDR_LEN], &zms_stage[zms_stage_offset], to_send);

    atomic_set(&bulk_in_flight, true);
    int err = bulk_notify_once(c, bulk_pkt, (uint16_t)(BULK_HDR_LEN + to_send));
    if (err == 0) {
        zms_stage_offset += to_send;
        bulk_seq++;
    } else {
        atomic_set(&bulk_in_flight, false);
    }
    return err;
}

/* ════════════════════════════════════════════════════════════════
 * Streaming state machine
 * ════════════════════════════════════════════════════════════════ */

/* Returns:
 *   0       — notification queued; next step fires via bulk_sent_cb
 *  -EAGAIN  — phase advanced, no notification sent; reschedule immediately
 *  -ENOMEM  — BLE stack backpressure; retry after 10ms
 *  other<0  — error; streaming stopped */
static int stream_send_one(struct bt_conn *c)
{
    uint16_t cap = max_payload_bytes(c);
    if (cap == 0U) {
        return -EINVAL;
    }

    switch (cur_phase) {

    case PHASE_ZMS_STAMP:
        if (zms_stage_offset >= zms_stage_len) {
            if (!load_next_zms_stamp()) {
                cur_phase = PHASE_RAM_STAMP;
                return -EAGAIN;
            }
        }
        return send_from_stage(c, STREAM_ID_STAMP, cap);

    case PHASE_RAM_STAMP: {
        /* Align capacity to int32_t boundary */
        uint16_t stamp_cap = (cap / 4U) * 4U;
        if (stamp_cap == 0U) {
            return -EINVAL;
        }

        k_spinlock_key_t skey = k_spin_lock(&rb_stamp_lock);
        uint8_t  *rb_ptr = NULL;
        uint32_t  claimed = ring_buf_get_claim(&rb_stamp, &rb_ptr, stamp_cap);
        k_spin_unlock(&rb_stamp_lock, skey);

        if (claimed == 0U) {
            int err = send_header_only(c, STREAM_ID_STAMP, BULK_FLAG_LAST);
            if (err == 0) {
                cur_phase        = PHASE_ZMS_AMBIENT;
                zms_stage_len    = 0U;
                zms_stage_offset = 0U;
            }
            return err;
        }

        /* rb_stamp_put only writes in 4-byte multiples; partial records
         * should never occur but guard against it explicitly */
        uint32_t aligned = (claimed / 4U) * 4U;
        if (aligned == 0U) {
            LOG_ERR("stamp RAM tail has partial record (%u bytes), discarding", claimed);
            skey = k_spin_lock(&rb_stamp_lock);
            (void)ring_buf_get_finish(&rb_stamp, 0U);
            k_spin_unlock(&rb_stamp_lock, skey);
            int err = send_header_only(c, STREAM_ID_STAMP, BULK_FLAG_LAST);
            if (err == 0) {
                cur_phase        = PHASE_ZMS_AMBIENT;
                zms_stage_len    = 0U;
                zms_stage_offset = 0U;
            }
            return err;
        }

        bulk_pkt[0] = STREAM_ID_STAMP;
        sys_put_le16(bulk_seq, &bulk_pkt[1]);
        bulk_pkt[3] = 0U;
        memcpy(&bulk_pkt[BULK_HDR_LEN], rb_ptr, aligned);

        atomic_set(&bulk_in_flight, true);
        int err = bulk_notify_once(c, bulk_pkt, (uint16_t)(BULK_HDR_LEN + aligned));

        skey = k_spin_lock(&rb_stamp_lock);
        (void)ring_buf_get_finish(&rb_stamp, (err == 0) ? aligned : 0U);
        k_spin_unlock(&rb_stamp_lock, skey);

        if (err == 0) {
            bulk_seq++;
        } else {
            atomic_set(&bulk_in_flight, false);
            if (err != -ENOMEM) {
                atomic_set(&bulk_stream_active, false);
            }
        }
        return err;
    }

    case PHASE_ZMS_AMBIENT:
        if (zms_stage_offset >= zms_stage_len) {
            if (!load_next_zms_ambient()) {
                cur_phase = PHASE_RAM_AMBIENT;
                return -EAGAIN;
            }
        }
        return send_from_stage(c, STREAM_ID_AMBIENT, cap);

    case PHASE_RAM_AMBIENT: {
        k_spinlock_key_t skey = k_spin_lock(&rb_ambient_lock);
        uint8_t  *rb_ptr = NULL;
        uint32_t  claimed = ring_buf_get_claim(&rb_ambient, &rb_ptr, cap);
        k_spin_unlock(&rb_ambient_lock, skey);

        if (claimed == 0U) {
            int err = send_header_only(c, STREAM_ID_AMBIENT, BULK_FLAG_LAST);
            if (err == 0) {
                cur_phase = PHASE_DONE;
                atomic_set(&bulk_stream_active, false);
            }
            return err;
        }

        bulk_pkt[0] = STREAM_ID_AMBIENT;
        sys_put_le16(bulk_seq, &bulk_pkt[1]);
        bulk_pkt[3] = 0U;
        memcpy(&bulk_pkt[BULK_HDR_LEN], rb_ptr, claimed);

        atomic_set(&bulk_in_flight, true);
        int err = bulk_notify_once(c, bulk_pkt, (uint16_t)(BULK_HDR_LEN + claimed));

        skey = k_spin_lock(&rb_ambient_lock);
        (void)ring_buf_get_finish(&rb_ambient, (err == 0) ? claimed : 0U);
        k_spin_unlock(&rb_ambient_lock, skey);

        if (err == 0) {
            bulk_seq++;
        } else {
            atomic_set(&bulk_in_flight, false);
            if (err != -ENOMEM) {
                atomic_set(&bulk_stream_active, false);
            }
        }
        return err;
    }

    default:
        return -EINVAL;
    }
}

/* ════════════════════════════════════════════════════════════════
 * Stream work handler and public API
 * ════════════════════════════════════════════════════════════════ */

static void bulk_stream_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (atomic_get(&bulk_in_flight)) {
        if (k_uptime_get() - stream_last_sent_ms > 2000) {
            LOG_WRN("stream: bulk_in_flight stuck >2s, clearing");
            atomic_set(&bulk_in_flight, false);
        } else {
            return;
        }
    }

    if (!atomic_get(&bulk_stream_active) || atomic_get(&bulk_in_flight)) {
        return;
    }

    /* Take a ref-counted handle so a disconnect racing this handler
     * cannot free the conn object under us */
    k_mutex_lock(&conn_mutex, K_FOREVER);
    struct bt_conn *c = conn ? bt_conn_ref(conn) : NULL;
    k_mutex_unlock(&conn_mutex);

    if (!c) {
        atomic_set(&bulk_stream_active, false);
        return;
    }

    int err = stream_send_one(c);
    bt_conn_unref(c);

    if (err == -EAGAIN) {
        stream_err_count = 0;
        k_work_schedule_for_queue(&stream_wq, &bulk_stream_work, K_NO_WAIT);
    } else if (err == -ENOMEM) {
        k_work_schedule_for_queue(&stream_wq, &bulk_stream_work, K_MSEC(10));
    } else if (err < 0) {
        stream_err_count++;
        LOG_WRN("stream error %d (attempt %u/%u)", err, stream_err_count, STREAM_MAX_ERR);
        if (stream_err_count >= STREAM_MAX_ERR) {
            LOG_ERR("stream aborted after %u errors", STREAM_MAX_ERR);
            atomic_set(&bulk_stream_active, false);
            stream_err_count = 0;
        } else {
            k_work_schedule_for_queue(&stream_wq, &bulk_stream_work,
                                      K_MSEC(50 * stream_err_count));
        }
    } else {
        stream_err_count = 0;
    }
}

void bulk_stream_init(void)
{
    k_work_queue_init(&stream_wq);
    k_work_queue_start(&stream_wq, stream_wq_stack,
                       K_THREAD_STACK_SIZEOF(stream_wq_stack),
                       K_PRIO_PREEMPT(10), NULL);

    k_work_init_delayable(&bulk_stream_work, bulk_stream_work_handler);
    atomic_set(&bulk_stream_active, false);
    atomic_set(&bulk_in_flight, false);

    int err = log_service_init();
    if (err) {
        LOG_WRN("ZMS init failed (%d) — RAM-only logging active", err);
    }
}

void bulk_stream_start(void)
{
    stream_err_count    = 0;
    stream_last_sent_ms = k_uptime_get();

    k_mutex_lock(&conn_mutex, K_FOREVER);
    bool connected = (conn != NULL);
    k_mutex_unlock(&conn_mutex);

    if (!connected || atomic_get(&bulk_stream_active)) {
        return;
    }

    /* Synchronously cancel commit work before snapshotting ZMS metadata */
    struct k_work_sync sync;
    k_work_cancel_sync(&stamp_commit_work,   &sync);
    k_work_cancel_sync(&ambient_commit_work, &sync);

    /* Freeze iteration boundaries so new commits during a download cannot
     * shift the endpoint */
    k_mutex_lock(&stamp_zms_mtx, K_FOREVER);
    zms_rkey_stamp    = stamp_meta.oldest_key;
    zms_end_key_stamp = stamp_meta.write_key;
    k_mutex_unlock(&stamp_zms_mtx);

    k_mutex_lock(&ambient_zms_mtx, K_FOREVER);
    zms_rkey_ambient    = ambient_meta.oldest_key;
    zms_end_key_ambient = ambient_meta.write_key;
    k_mutex_unlock(&ambient_zms_mtx);

    bulk_seq         = 0U;
    cur_phase        = PHASE_ZMS_STAMP;
    zms_stage_len    = 0U;
    zms_stage_offset = 0U;

    atomic_set(&bulk_stream_active, true);
    atomic_set(&bulk_in_flight, false);

    k_work_schedule_for_queue(&stream_wq, &bulk_stream_work, K_NO_WAIT);
}

void bulk_stream_stop(void)
{
    atomic_set(&bulk_stream_active, false);
    atomic_set(&bulk_in_flight, false);
    k_work_cancel_delayable(&bulk_stream_work);
}