/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "actuator.h"
#include "cloud.h"

LOG_MODULE_REGISTER(actuator);

/* How often the Hall sensor is sampled while the motor runs. */
#define MOTOR_POLL_MS	    1
/* Consecutive samples a new Hall level must hold to be accepted (debounce). */
#define HALL_DEBOUNCE_SAMPLES 3
/* Time after motor start during which the Hall is ignored. At rest the magnet
 * sits at the sensor; this blanking window lets the shaft move off home before
 * we start looking for the magnet's return, so start-up jitter cannot trigger
 * an immediate false stop.
 */
#define HALL_BLANKING_MS    250
/* Extra run time after the magnet is sensed. The shaft is still moving when
 * the magnet reaches the sensor, so a short overrun lets it rotate the rest
 * of the way to the proper resting spot.
 */
#define HALL_OVERRUN_MS	    50
/* Maximum motor run time. If the magnet is never sensed (jam, misaligned
 * magnet, faulty sensor) the motor is forced off so it cannot burn out.
 */
#define MOTOR_SAFETY_MS	    1500
/* Minimum time between flushes after the motor stops. */
#define FLUSH_LOCKOUT_MS    1000
/* Edges on the failsafe switch within this window are contact bounce. */
#define BUTTON_DEBOUNCE_MS  50

/* P1.06: HIGH = MOSFET on = motor runs (active high). */
static const struct gpio_dt_spec motor_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(flush_actuator), gpios);
/* P1.12: the sensor pulls the line low when the magnet is present (active
 * low, so a logical read of 1 means "magnet at sensor"). Moved off P1.07,
 * which the nRF7002 EB II claims as its coexistence GRANT pad.
 */
static const struct gpio_dt_spec hall_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(hall_sensor), gpios);
/* P1.08: failsafe flush switch to GND (active low, internal pull-up). */
static const struct gpio_dt_spec button_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(flush_button), gpios);
static struct gpio_callback button_cb;

static K_SEM_DEFINE(flush_sem, 0, 1);
static atomic_t motor_active;
/* k_uptime (ms) before which new flush requests are ignored. */
static int64_t lockout_until;

/* Debounced read of the Hall sensor: returns true when the magnet is present.
 * @p state holds the accepted level; @p cnt counts samples toward a change.
 */
static bool hall_present(bool *state, uint8_t *cnt)
{
	const bool raw = gpio_pin_get_dt(&hall_gpio) > 0;

	if (raw == *state) {
		*cnt = 0;
	} else if (++(*cnt) >= HALL_DEBOUNCE_SAMPLES) {
		*state = raw;
		*cnt = 0;
	}

	return *state;
}

static void motor_run(void)
{
	uint8_t cnt = 0;
	/* Seed the debounced state with the level at start-up. */
	bool present = gpio_pin_get_dt(&hall_gpio) > 0;

	(void)gpio_pin_set_dt(&motor_gpio, 1);
	const int64_t start = k_uptime_get();

	LOG_INF("Flush: motor on (magnet %s at start)", present ? "present" : "clear");

	while (true) {
		const int64_t elapsed = k_uptime_get() - start;

		if (elapsed > MOTOR_SAFETY_MS) {
			LOG_WRN("Flush: safety timeout (%d ms), forcing motor off",
				MOTOR_SAFETY_MS);
			break;
		}

		present = hall_present(&present, &cnt);

		/* Ignore the Hall during the blanking window, then stop the next
		 * time the magnet reaches the sensor (the home position).
		 */
		if (elapsed >= HALL_BLANKING_MS && present) {
			LOG_INF("Flush: magnet sensed at +%lld ms, motor off after %d ms overrun",
				elapsed, HALL_OVERRUN_MS);
			k_msleep(HALL_OVERRUN_MS);
			break;
		}

		k_msleep(MOTOR_POLL_MS);
	}

	(void)gpio_pin_set_dt(&motor_gpio, 0);
	lockout_until = k_uptime_get() + FLUSH_LOCKOUT_MS;
	atomic_set(&motor_active, 0);

	/* Report the completed flush to nRF Cloud (non-blocking). */
	cloud_report_flush();
}

static void motor_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		k_sem_take(&flush_sem, K_FOREVER);
		motor_run();
	}
}

K_THREAD_DEFINE(motor_tid, 1024, motor_thread_fn, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, 0);

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	static int64_t last_edge;
	const int64_t now = k_uptime_get();

	if (now - last_edge < BUTTON_DEBOUNCE_MS) {
		return;
	}
	last_edge = now;

	LOG_INF("Failsafe switch pressed");
	actuator_flush();
}

int actuator_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&motor_gpio) || !gpio_is_ready_dt(&hall_gpio)) {
		LOG_ERR("Actuator GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&motor_gpio, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure motor GPIO (err %d)", err);
		return err;
	}

	err = gpio_pin_configure_dt(&hall_gpio, GPIO_INPUT | GPIO_PULL_UP);
	if (err) {
		LOG_ERR("Failed to configure Hall GPIO (err %d)", err);
		return err;
	}

	if (!gpio_is_ready_dt(&button_gpio)) {
		LOG_ERR("Flush switch GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button_gpio, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure flush switch GPIO (err %d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&button_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to configure flush switch interrupt (err %d)", err);
		return err;
	}

	gpio_init_callback(&button_cb, button_pressed, BIT(button_gpio.pin));

	err = gpio_add_callback_dt(&button_gpio, &button_cb);
	if (err) {
		LOG_ERR("Failed to add flush switch callback (err %d)", err);
		return err;
	}

	return 0;
}

void actuator_flush(void)
{
	const int64_t now = k_uptime_get();

	if (atomic_get(&motor_active)) {
		LOG_INF("Flush ignored (motor already running)");
		return;
	}

	if (now < lockout_until) {
		LOG_INF("Flush ignored (lockout, %lld ms left)", lockout_until - now);
		return;
	}

	atomic_set(&motor_active, 1);
	k_sem_give(&flush_sem);
}
