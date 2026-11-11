/*
 * time_service.c — AZSensorSuite firmware, timekeeping
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file time_service.c
 * @brief Wall-clock timekeeping.
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include "time_service.h"

/* The BLE write callback and sensor/schedule work can access this concurrently. */
K_MUTEX_DEFINE(time_mutex);
static int64_t          unix_base_time_ms = 0;
static time_changed_cb_t time_changed_cb  = NULL;

void set_real_time(int64_t current_unix_time_ms)
{
    k_mutex_lock(&time_mutex, K_FOREVER);
    unix_base_time_ms = current_unix_time_ms - k_uptime_get();
    k_mutex_unlock(&time_mutex);

    if (time_changed_cb) {
        time_changed_cb();
    }
}

int64_t get_current_timestamp(void)
{
    k_mutex_lock(&time_mutex, K_FOREVER);
    int64_t timestamp = unix_base_time_ms + k_uptime_get();
    k_mutex_unlock(&time_mutex);
    return timestamp;
}

int time_service_init(time_changed_cb_t cb)
{
    time_changed_cb = cb;
    return 0;
}
