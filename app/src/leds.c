/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(leds);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* Set while a wake-word detection blink owns LED1, so the optional heartbeat
 * pulse (see CONFIG_APP_LED_HEARTBEAT below) does not cut it short.
 */
static atomic_t led1_blink_active;

static void led_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(led_timer, led_timer_expiry, NULL);

static int led_init(const struct gpio_dt_spec *spec)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO %s is not ready", spec->port->name);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure GPIO %s pin %u (err %d)", spec->port->name, spec->pin,
			err);
		return err;
	}

	return 0;
}

int leds_init(void)
{
	int err;

	err = led_init(&led0);
	if (err) {
		return err;
	}

	err = led_init(&led1);
	if (err) {
		return err;
	}

	return 0;
}

void leds_blink_led0(void)
{
	gpio_pin_set_dt(&led0, 1);
	k_timer_user_data_set(&led_timer, (void *)&led0);
	k_timer_start(&led_timer, K_SECONDS(1), K_NO_WAIT);
}

void leds_blink_led1(void)
{
	atomic_set(&led1_blink_active, 1);
	gpio_pin_set_dt(&led1, 1);
	k_timer_user_data_set(&led_timer, (void *)&led1);
	k_timer_start(&led_timer, K_SECONDS(1), K_NO_WAIT);
}

static void led_timer_expiry(struct k_timer *timer)
{
	const struct gpio_dt_spec *led = k_timer_user_data_get(timer);

	gpio_pin_set_dt(led, 0);

	if (led == &led1) {
		atomic_set(&led1_blink_active, 0);
	}
}

void leds_on_led0(void)
{
	gpio_pin_set_dt(&led0, 1);
}

void leds_off_led0(void)
{
	gpio_pin_set_dt(&led0, 0);
}

void leds_on_led1(void)
{
	gpio_pin_set_dt(&led1, 1);
}

void leds_off_led1(void)
{
	gpio_pin_set_dt(&led1, 0);
}

#if defined(CONFIG_APP_LED_HEARTBEAT)
/* Periodic "alive" pulse instead of holding LED1 on for the whole uptime.
 * A DK LED draws on the order of a couple of mA; held continuously that is a
 * larger average draw than everything the connectivity duty cycle saves, and
 * on the bench it adds a constant offset to every current trace. At the
 * default 20 ms / 5 s the duty cycle is 0.4%, so the average collapses by
 * ~250x while the board still visibly says "running".
 *
 * Two timers rather than one: the periodic timer owns the ON edge and arms a
 * one-shot for the OFF edge. Both expiries run in ISR context, which is fine
 * because gpio_pin_set_dt() on nRF GPIO is a register write.
 */
static void hb_on_expiry(struct k_timer *timer);
static void hb_off_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(hb_on_timer, hb_on_expiry, NULL);
static K_TIMER_DEFINE(hb_off_timer, hb_off_expiry, NULL);

static void hb_on_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	/* A wake-word blink owns LED1 for a full second. Skip the pulse rather
	 * than truncate it -- the detection blink is the signal a user is
	 * actually looking for.
	 */
	if (atomic_get(&led1_blink_active)) {
		return;
	}

	gpio_pin_set_dt(&led1, 1);
	k_timer_start(&hb_off_timer, K_MSEC(CONFIG_APP_LED_HEARTBEAT_ON_MS), K_NO_WAIT);
}

static void hb_off_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	/* A detection blink started inside our pulse: let it own the LED. */
	if (atomic_get(&led1_blink_active)) {
		return;
	}

	gpio_pin_set_dt(&led1, 0);
}

void leds_heartbeat_start(void)
{
	k_timer_start(&hb_on_timer, K_MSEC(CONFIG_APP_LED_HEARTBEAT_INTERVAL_MS),
		      K_MSEC(CONFIG_APP_LED_HEARTBEAT_INTERVAL_MS));
}
#endif /* CONFIG_APP_LED_HEARTBEAT */
