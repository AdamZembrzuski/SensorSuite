/*
 * error_service.h — AZSensorSuite firmware, error reporting
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file error_service.h
 * @brief Fatal error handling and diagnostic LED signalling.
 *
 * Provides error_fatal(), which disables the hardware watchdog, flushes
 * the log, and enters a non-returning blink loop that encodes the fault type
 * as a visible LED frequency. Also overrides k_sys_fatal_error_handler to
 * catch kernel panics with the same blink scheme. Blink frequencies are
 * defined per fatal_code_t so the fault can be identified without a
 * debugger attached.
 */

#ifndef ERROR_SERVICE_H
#define ERROR_SERVICE_H

#include <zephyr/drivers/gpio.h>

/* ════════════════════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Fatal error codes used by the application.
 *
 * Each value maps to a distinct blink frequency on the error LED,
 * allowing the fault to be identified without a debugger attached.
 */
typedef enum {
    FATAL_GPIO_INIT      = 1, /**< GPIO initialisation failure.         */
    FATAL_VL53_INIT      = 2, /**< VL53L4CD initialisation failure.     */
    FATAL_VL53_CLEAR_IRQ = 3, /**< VL53L4CD interrupt clear failure.    */
    FATAL_ZMS_INIT       = 4, /**< ZMS partition initialisation failure. */
    FATAL_ZMS_MOUNT      = 5, /**< ZMS filesystem mount failure.         */
    FATAL_BT_INIT        = 6, /**< Bluetooth stack initialisation failure. */
    FATAL_KERNEL         = 7, /**< Kernel panic or fatal error handler.  */
} fatal_code_t;

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the error service with the given error LED.
 *
 * Must be called before error_fatal(). Configures the GPIO pin and
 * verifies the LED port is ready. Subsequent calls to error_fatal()
 * will use the LED registered here.
 *
 * @param err_led  GPIO specification for the error LED.
 *
 * @retval 0        Success.
 * @retval -EINVAL  @p err_led is NULL.
 * @retval -ENODEV  LED port is not ready.
 * @return          Negative GPIO error code on other failures.
 */
int error_service_init(const struct gpio_dt_spec *err_led);

/**
 * @brief Trigger a fatal application error and enter the blink loop.
 *
 * Disables the hardware watchdog, flushes the log, then blinks the
 * error LED indefinitely at a frequency determined by @p code.
 *
 * @param code  Fatal error code to display on the error LED.
 *
 * @note This function does not return.
 */
__attribute__((noreturn))
void error_fatal(fatal_code_t code);

#endif /* ERROR_SERVICE_H */