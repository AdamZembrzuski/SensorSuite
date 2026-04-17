/*
 * sense_service.h — AZSensorSuite firmware, GATT service
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file sense_service.h
 * @brief GATT service declaration and characteristic handlers.
 *
 * Defines the primary BLE service with characteristics for object count,
 * temperature, humidity, bulk notification stream, current timestamp, base
 * timestamp (encrypted read/write), and command (encrypted read/write).
 * Read handlers invoke application callbacks registered via sense_service_init.
 * The timestamp write handler sets the firmware wall-clock via time_service.
 */

#ifndef SENSE_SERVICE_H
#define SENSE_SERVICE_H

#include <zephyr/types.h>

/* ════════════════════════════════════════════════════════════════
 * Definitions
 * ════════════════════════════════════════════════════════════════ */

/* Base UUID: 229a0000-ad33-4a06-9bce-c34201743655 */

#define BT_SERVICE_UUID_VAL        BT_UUID_128_ENCODE(0x229a0001, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_COUNT_UUID_VAL          BT_UUID_128_ENCODE(0x229a0002, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_TEMP_UUID_VAL           BT_UUID_128_ENCODE(0x229a0003, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_HUMIDITY_UUID_VAL       BT_UUID_128_ENCODE(0x229a0004, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_BULK_UUID_VAL           BT_UUID_128_ENCODE(0x229a0005, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_CURR_TIMESTAMP_UUID_VAL BT_UUID_128_ENCODE(0x229a0006, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_BASE_TIMESTAMP_UUID_VAL BT_UUID_128_ENCODE(0x229a0007, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_COMMAND_UUID_VAL        BT_UUID_128_ENCODE(0x229a0008, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)

#define BT_SERVICE_UUID            BT_UUID_DECLARE_128(BT_SERVICE_UUID_VAL)
#define BT_COUNT_UUID              BT_UUID_DECLARE_128(BT_COUNT_UUID_VAL)
#define BT_TEMP_UUID               BT_UUID_DECLARE_128(BT_TEMP_UUID_VAL)
#define BT_HUMIDITY_UUID           BT_UUID_DECLARE_128(BT_HUMIDITY_UUID_VAL)
#define BT_BULK_UUID               BT_UUID_DECLARE_128(BT_BULK_UUID_VAL)
#define BT_CURR_TIMESTAMP_UUID     BT_UUID_DECLARE_128(BT_CURR_TIMESTAMP_UUID_VAL)
#define BT_BASE_TIMESTAMP_UUID     BT_UUID_DECLARE_128(BT_BASE_TIMESTAMP_UUID_VAL)
#define BT_COMMAND_UUID            BT_UUID_DECLARE_128(BT_COMMAND_UUID_VAL)

/* ════════════════════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════════════════════ */

/** @brief Called on a count characteristic read; returns the current object count. */
typedef uint32_t (*count_cb_t)(void);

/** @brief Called on a temperature characteristic read; returns temperature in centidegrees (°C × 100). */
typedef int32_t (*temp_cb_t)(void);

/** @brief Called on a humidity characteristic read; returns humidity in centi-percent (%RH × 100). */
typedef uint32_t (*humid_cb_t)(void);

/**
 * @brief Called on a command characteristic write; dispatches the command byte.
 *
 * @param command  Raw command byte written by the BLE host.
 * @return         Response byte stored back into the command characteristic.
 */
typedef uint8_t (*cmnd_cb_t)(uint8_t command);

/**
 * @brief Application callbacks registered with the sense service.
 *
 * All fields are optional; set unused callbacks to NULL. Read handlers
 * return BT_ATT_ERR_UNLIKELY if the corresponding callback is NULL.
 */
struct sense_cb {
    count_cb_t count_cb; /**< Object count read handler.    */
    temp_cb_t  temp_cb;  /**< Temperature read handler.     */
    humid_cb_t humid_cb; /**< Humidity read handler.        */
    cmnd_cb_t  cmnd_cb;  /**< Command characteristic write. */
};

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the sense service with application callbacks.
 *
 * Must be called after bt_enable() and before any characteristic is
 * accessed by a connected peer. The callback struct is copied internally;
 * the caller does not need to keep it alive after this call returns.
 *
 * @param callbacks  Pointer to a populated sense_cb struct.
 *
 * @retval 0        Success.
 * @retval -EINVAL  @p callbacks is NULL.
 */
int sense_service_init(struct sense_cb *callbacks);

/**
 * @brief Send a notification on the object count characteristic.
 *
 * @param object_count  Current object count value.
 *
 * @retval 0        Success.
 * @retval -EACCES  Notifications are not enabled by the connected peer.
 */
int sense_service_object_count_notify(uint32_t object_count);

/**
 * @brief Send a notification on the temperature characteristic.
 *
 * @param temperature  Temperature in centidegrees (°C × 100).
 *
 * @retval 0        Success.
 * @retval -EACCES  Notifications are not enabled by the connected peer.
 */
int sense_service_temperature_notify(int32_t temperature);

/**
 * @brief Send a notification on the humidity characteristic.
 *
 * @param humidity  Humidity in centi-percent (%RH × 100).
 *
 * @retval 0        Success.
 * @retval -EACCES  Notifications are not enabled by the connected peer.
 */
int sense_service_humidity_notify(uint32_t humidity);

/**
 * @brief Send a notification on the bulk data characteristic.
 *
 * Used by the log service bulk-stream state machine to deliver sequenced
 * download packets to the BLE host.
 *
 * @param data  Pointer to the packet buffer (header + payload).
 * @param len   Total packet length in bytes.
 *
 * @retval 0        Success.
 * @retval -EACCES  Notifications are not enabled by the connected peer.
 * @retval -EINVAL  @p data is NULL or @p len is 0.
 */
int sense_service_bulk_notify(const uint8_t *data, uint16_t len);

#endif /* SENSE_SERVICE_H */