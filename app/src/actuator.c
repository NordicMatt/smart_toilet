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
/* Maximum motor run time. If the magnet is never sensed (jam, misaligned
 * magnet, faulty sensor) the motor is forced off so it cannot burn out.
 */
#define MOTOR_SAFETY_MS	    1000
/* Minimum time between flushes after the motor stops. */
#define FLUSH_LOCKOUT_MS    3000

/* P1.06: HIGH = MOSFET on = motor runs (active high). */
static const struct gpio_dt_spec motor_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(flush_actuator), gpios);
/* P1.07: the sensor pulls the line low when the magnet is present (active
 * low, so a logical read of 1 means "magnet at sensor").
 */
static const struct gpio_dt_spec hall_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(hall_sensor), gpios);

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
			LOG_INF("Flush: magnet sensed at +%lld ms, motor off", elapsed);
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
