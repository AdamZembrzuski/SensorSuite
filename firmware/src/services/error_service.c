#include "error_service.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/arch/exception.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/watchdog.h>

static const struct device *wdt_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));


static struct gpio_dt_spec s_err_led;
static bool s_err_led_valid;
static volatile fatal_code_t s_fatal_code = FATAL_KERNEL; // Volatile for debug

static uint32_t half_period_us_for_code(fatal_code_t code)
{
	switch (code) {
	case FATAL_GPIO_INIT:      return 500000; /* 1 Hz */
	case FATAL_VL53_ID:        return 250000; /* 2 Hz */
	case FATAL_VL53_INIT:      return 166667; /* 3 Hz */
	case FATAL_VL53_START:     return 125000; /* 4 Hz */
	case FATAL_VL53_CLEAR_IRQ: return 100000; /* 5 Hz */
	case FATAL_BT_INIT:        return  83333; /* 6 Hz */
	case FATAL_KERNEL:         return  71429; /* 7 Hz */
	default:                   return 500000;
	}
}

__attribute__((noreturn))
static void blink_fatal_forever(fatal_code_t code)
{
	uint32_t half_period_us = half_period_us_for_code(code);

	if (s_err_led_valid) {
		(void)gpio_pin_configure_dt(&s_err_led, GPIO_OUTPUT_INACTIVE);
	}

	for (;;) {
		if (s_err_led_valid) {
			gpio_pin_set_dt(&s_err_led, 1);
		}
		k_busy_wait(half_period_us);

		if (s_err_led_valid) {
			gpio_pin_set_dt(&s_err_led, 0);
		}
		k_busy_wait(half_period_us);
	}
}

int error_service_init(const struct gpio_dt_spec *err_led)
{
	if (!err_led) {
		return -EINVAL;
	}

	if (!device_is_ready(err_led->port)) {
		return -ENODEV;
	}

	s_err_led = *err_led;
	s_err_led_valid = true;

	return gpio_pin_configure_dt(&s_err_led, GPIO_OUTPUT_INACTIVE);
}

__attribute__((noreturn))
void error_fatal(fatal_code_t code)
{	

	if (wdt_dev && device_is_ready(wdt_dev)) {
        wdt_disable(wdt_dev);
    }

	if (s_err_led_valid) {
        gpio_pin_set_dt(&s_err_led, 1);
    }

	k_busy_wait(2 * 1000 * 1000); // 2 seconds


	LOG_PANIC();
	(void)irq_lock();

	s_fatal_code = code;

	if (s_err_led_valid) {
		gpio_pin_set_dt(&s_err_led, 1);
	}

	blink_fatal_forever(code);
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);

	s_fatal_code = FATAL_KERNEL;
	LOG_PANIC();
	blink_fatal_forever(FATAL_KERNEL);
}