/*
 * log_service.h — AZSensorSuite firmware, sensor log and BLE bulk stream
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file log_service.h
 * @brief Detection log storage and BLE bulk download stream.
 *
 * Maintains two hybrid stores — stamp (int32_t delta-second timestamps)
 * and ambient (temperature/humidity pairs) — each backed by a ZMS RRAM
 * partition with a RAM ring buffer as a fast ingestion tail. Commit work
 * drains full chunks from the ring buffers to ZMS in the background. The
 * bulk stream exhausts persisted ZMS chunks then the live RAM tail across a
 * four-phase state machine, delivered as sequenced BLE notifications on a
 * dedicated workqueue. Also owns power-loss state persistence via a reserved
 * ZMS key.
 */

#ifndef LOG_SERVICE_H
#define LOG_SERVICE_H

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════════════════════
 * Definitions
 * ════════════════════════════════════════════════════════════════ */

/* RAM ring-buffer sizes (live tail only)
 *
 * These hold data not yet flushed to ZMS. Intentionally small; the bulk of
 * historical data lives in the ZMS partitions. Must be power-of-two for
 * ring_buf macros.
 */
#define RB_STAMP_SIZE   (4U * 1024U)   /* bytes: ~1 024 int32_t timestamps  */
#define RB_AMBIENT_SIZE (1U * 1024U)   /* bytes: ~512 temp/humidity pairs   */

/* ZMS chunk sizes
 *
 * A "chunk" is one ZMS entry. When the RAM ring buffer accumulates this
 * many items the commit-work handler drains them into a single ZMS write.
 *
 * Stamp entry size   = ZMS_STAMP_CHUNK_SIZE   * sizeof(int32_t)        = 128 B
 * Ambient entry size = ZMS_AMBIENT_CHUNK_SIZE * sizeof(ambient_sample) =  64 B
 */
#define ZMS_STAMP_CHUNK_SIZE   32U
#define ZMS_AMBIENT_CHUNK_SIZE 32U

/* ZMS sector size
 *
 * nRF54L15 RRAM write granularity is 16 B; there is no traditional erase.
 * 2 048 B sectors work well for ZMS on this device.
 *   Stamp  ZMS: 480 KB / 2 KB = 240 sectors
 *   Ambient ZMS:  32 KB / 2 KB =  16 sectors
 */
#define LOG_ZMS_SECTOR_SIZE    2048U

/* Full-log strategy
 *
 * STOP_WHEN_FULL   — preserve oldest data; new chunks dropped when ZMS full.
 * OVERWRITE_OLDEST — circular log; oldest chunk deleted to make room.
 */
#define STOP_WHEN_FULL   0
#define OVERWRITE_OLDEST 1

#ifndef LOG_FULL_STRATEGY
#define LOG_FULL_STRATEGY OVERWRITE_OLDEST
#endif

/* BLE bulk-stream protocol constants */
#define BULK_HDR_LEN      4U
#define BULK_FLAG_LAST    0x01U
#define STREAM_ID_STAMP   1U
#define STREAM_ID_AMBIENT 2U
#define BULK_PKT_MAX      256U

/* ════════════════════════════════════════════════════════════════
 * Public API — ingestion (called from main.c sensor loops)
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Return true if a bulk download is currently in progress.
 *
 * Safe to call from any context. Used by the sensor loops in main.c to
 * suppress new ingestion while a download is active.
 */
bool bulk_stream_is_active(void);

/**
 * @brief Append a delta-time stamp to the log pipeline.
 *
 * Data lands in the RAM ring buffer first. When ZMS_STAMP_CHUNK_SIZE
 * entries have accumulated the commit-work handler flushes them to RRAM.
 *
 * @param value  Delta-time in seconds (int32_t).
 *
 * @retval 0       Success.
 * @retval -ENOSPC RAM tail is full.
 */
int rb_stamp_put(int32_t value);

/**
 * @brief Retrieve one timestamp from the RAM tail (streaming only).
 *
 * @param value  Output pointer.
 *
 * @retval 0       Success.
 * @retval -ENOENT Ring buffer is empty.
 */
int rb_stamp_get(int32_t *value);

/**
 * @brief Append a temp/humidity sample pair to the log pipeline.
 *
 * @param data  Two bytes: data[0] = int8_t temp °C, data[1] = uint8_t %RH.
 * @param len   Must be 2.
 *
 * @retval 0       Success.
 * @retval -ENOSPC RAM tail is full.
 */
int rb_ambient_put(const uint8_t *data, size_t len);

/**
 * @brief Retrieve ambient bytes from the RAM tail (streaming only).
 *
 * @param data  Output buffer.
 * @param len   Bytes to retrieve.
 *
 * @retval 0       Success.
 * @retval -ENOENT Insufficient data in the ring buffer.
 */
int rb_ambient_get(uint8_t *data, size_t len);

/* ════════════════════════════════════════════════════════════════
 * Public API — count helpers
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Total timestamp records across ZMS storage and the RAM tail.
 *
 * Safe to call from the BT RX thread. Uses an atomic counter mirror;
 * does not acquire any mutex.
 */
uint32_t log_total_stamp_count(void);

/**
 * @brief Total ambient sample pairs across ZMS storage and the RAM tail.
 */
uint32_t log_total_ambient_count(void);

/**
 * @brief RAM-tail stamp count only (bytes / sizeof(int32_t)).
 */
uint32_t rb_stamp_count(void);

/* ════════════════════════════════════════════════════════════════
 * Public API — persist state
 * ════════════════════════════════════════════════════════════════ */

/*
 * Provides reboot-safe persistence for the counter state and wall-clock time.
 *
 * Without this, every power cycle loses:
 *   last_event_unix — first post-reset detection delta is always 0
 *   carry_ms        — sub-second accumulation error accumulates on reboot
 *   real time       — user must re-write BT_BASE_TIMESTAMP_UUID each boot
 *
 * Implementation: a compact record stored at a reserved ZMS key (0xFFFF0000)
 * in the stamp partition — outside the monotonically incrementing data key
 * range and never touched by the OVERWRITE_OLDEST eviction loop. Always
 * written to the same key, so it is an in-place overwrite with no storage
 * accumulation.
 *
 * Typical call sites in main.c:
 *   log_persist_state()  — after rb_stamp_put() in counter_work_handler,
 *                          and after the SHT30 read in ambient_work_handler
 *                          (keeps wall-clock fresh even without detections).
 *   log_restore_state()  — once in main(), after bulk_stream_init().
 */

/**
 * @brief Persist counter state and current wall-clock time to RRAM.
 *
 * Best-effort — a non-zero return should be logged but not treated as fatal.
 * No-op if ZMS is not ready. Must not be called from ISR context or with
 * stamp_zms_mtx already held.
 *
 * @param last_event_unix_ms  Unix timestamp (ms) of the last logged detection.
 *                            Pass 0 if no detection has occurred this session.
 * @param carry_ms            Sub-second remainder from delta accumulation.
 * @param current_time_ms     Current time from get_current_timestamp().
 *
 * @retval 0  Success.
 * @return    Negative errno on ZMS write failure.
 */
int log_persist_state(int64_t last_event_unix_ms,
                      int64_t carry_ms,
                      int64_t current_time_ms);

/**
 * @brief Restore counter state and wall-clock seed from RRAM.
 *
 * Called once at boot, after bulk_stream_init(). If a valid record is found
 * all three output pointers are populated.
 *
 * @note The seeded_time_ms value will be stale by the power-off duration.
 *       This is acceptable — it lets the schedule make a reasonable decision
 *       until the user sets accurate time over BLE.
 *
 * @param last_event_unix_ms  Restored Unix timestamp of the last detection.
 * @param carry_ms            Restored sub-second remainder.
 * @param seeded_time_ms      Last persisted wall-clock time; pass to
 *                            set_real_time() to seed the clock at boot.
 *
 * @retval true   A valid persist record was found and restored.
 * @retval false  No valid record found; outputs are unchanged.
 */
bool log_restore_state(int64_t *last_event_unix_ms,
                       int64_t *carry_ms,
                       int64_t *seeded_time_ms);

/* ════════════════════════════════════════════════════════════════
 * Public API — service lifecycle
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the log service and mount both ZMS instances.
 *
 * Called internally by bulk_stream_init(); not typically called directly.
 *
 * @retval 0  Success.
 * @return    Negative errno on ZMS mount failure.
 */
int log_service_init(void);

/**
 * @brief Erase all persisted log data and reset the service.
 *
 * Clears both ZMS partitions (including the persist-state record), resets
 * all write pointers, and empties the RAM ring buffers.
 *
 * @note Must only be called while sensor ingestion is paused
 *       (logging_paused = true in main.c).
 */
void log_service_clear(void);

/* ════════════════════════════════════════════════════════════════
 * Public API — BLE bulk stream
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the bulk-stream workqueue and mount ZMS.
 *
 * Must be called once at boot before log_restore_state() or
 * bulk_stream_start().
 */
void bulk_stream_init(void);

/**
 * @brief Begin a full chronological download (ZMS then RAM tail).
 *
 * Snapshots the ZMS iteration boundaries, resets the sequence counter,
 * and schedules the stream work. No-op if already streaming or if no
 * peer is connected.
 */
void bulk_stream_start(void);

/**
 * @brief Abort an in-progress bulk download.
 *
 * Clears the active and in-flight flags and cancels any pending stream work.
 * Safe to call when no download is active.
 */
void bulk_stream_stop(void);

#endif /* LOG_SERVICE_H */