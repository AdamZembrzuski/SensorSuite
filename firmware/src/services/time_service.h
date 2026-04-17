/*
 * time_service.h — AZSensorSuite firmware, timekeeping
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file time_service.h
 * @brief Wall-clock timekeeping.
 *
 * The time base is derived from a Unix millisecond timestamp written by the
 * BLE host and the kernel uptime counter. A single writer sets the base via
 * @ref set_real_time; subsequent calls to @ref get_current_timestamp derive
 * the current time from the stored offset and @c k_uptime_get(). A registered
 * callback allows the schedule to recompute its next transition immediately
 * when the time is set. No internal locking is applied — the caller is
 * responsible for ensuring @ref set_real_time is not called concurrently.
 */

#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <zephyr/types.h>

/* ════════════════════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Callback invoked whenever the system time base is updated.
 *
 * Registered via @ref time_service_init and called from @ref set_real_time
 * each time a new time base is written. May be @c NULL if no notification
 * is required.
 */
typedef void (*time_changed_cb_t)(void);

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the time service and register the change callback.
 *
 * Must be called before @ref set_real_time or @ref get_current_timestamp.
 * Passing @c NULL for @p cb disables change notifications.
 *
 * @param cb  Function to call whenever the time base changes, or @c NULL.
 *
 * @retval 0  Always succeeds.
 */
int time_service_init(time_changed_cb_t cb);

/**
 * @brief Set the system time base from a known Unix epoch value.
 *
 * Stores @p current_unix_time_ms minus the current @c k_uptime_get() as the
 * base offset, then fires the registered @ref time_changed_cb_t (if any).
 *
 * @note This is the sole writer of the time base. The caller must ensure
 *       it is not invoked concurrently from multiple contexts.
 *
 * @param current_unix_time_ms  Current wall-clock time in milliseconds since
 *                              the Unix epoch (1970-01-01T00:00:00Z).
 */
void set_real_time(int64_t current_unix_time_ms);

/**
 * @brief Return the current time as milliseconds since the Unix epoch.
 *
 * Computed as the stored base offset plus @c k_uptime_get(). Returns zero
 * (i.e. epoch) if @ref set_real_time has not been called yet; callers should
 * check @ref is_time_set before relying on the result.
 *
 * @return Current Unix timestamp in milliseconds, or 0 if the time base
 *         has not been set.
 */
int64_t get_current_timestamp(void);

#endif /* TIME_SERVICE_H */