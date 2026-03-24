#ifndef ERROR_SERVICE_H_
#define ERROR_SERVICE_H_

#include <zephyr/drivers/gpio.h>

/** @brief Fatal error codes used by the application
 *
 * Each code maps to a distinct blink frequency on the error LED.
 */
typedef enum {
	FATAL_GPIO_INIT        = 1,
	FATAL_VL53_ID          = 2,
	FATAL_VL53_INIT        = 3,
	FATAL_VL53_START       = 4,
	FATAL_VL53_CLEAR_IRQ   = 5,
	FATAL_BT_INIT          = 6,
	FATAL_KERNEL           = 7,
} fatal_code_t;

/** @brief Initialises the error service with the given error LED
 * @param err_led A pointer to the GPIO specification for the error LED
 * @return 0 on success, -EINVAL if err_led is NULL, -ENODEV if the
 * LED port is not ready, or a negative GPIO error code on failure
 */
int error_service_init(const struct gpio_dt_spec *err_led);

/** @brief Triggers a fatal application error and enters the fatal blink loop
 * @param code The fatal error code to display
 * @note This function does not return
 */
__attribute__((noreturn))
void error_fatal(fatal_code_t code);

#endif