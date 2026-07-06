/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Memfault connectivity (HTTPS over Wi-Fi) for the smart toilet.
 *
 * The device talks only to Memfault: diagnostics (coredumps, reboot reasons,
 * metrics) are POSTed to Memfault over HTTPS, and FOTA is fetched directly from
 * Memfault Release Management (also HTTPS). No nRF Cloud connection — a single
 * cloud relationship, one TLS connection at a time.
 *
 * Runs in its own thread so the wake-word/DMIC loop in main.c is untouched:
 *   conn_mgr brings up Wi-Fi (stored credentials) -> wait for L4 -> obtain time
 *   via NTP (TLS cert validity) -> periodically upload Memfault data and check
 *   Memfault for a FOTA update. Memfault root certs are provisioned at boot via
 *   CONFIG_MEMFAULT_NCS_PROVISION_CERTIFICATES.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>

#include <date_time.h>

#include <memfault/core/data_packetizer.h>
#include <memfault/core/reboot_tracking.h>
#include <memfault/panics/assert.h>
#include <memfault/metrics/connectivity.h>
#include <memfault/metrics/metrics.h>
#include <memfault/ports/zephyr/http.h>
#include <memfault/ports/zephyr/fota.h>

#include "cloud.h"

LOG_MODULE_REGISTER(cloud, LOG_LEVEL_INF);

#define L4_EVENT_MASK	    (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define DATE_TIME_TIMEOUT_S 30
/* How often, while connected, to drain pending Memfault data to the cloud. */
#define UPLOAD_TICK_S	    15
/* How often, while connected, to ask Memfault whether a FOTA update is ready. */
#define FOTA_CHECK_S	    120
/* Reboot if the cloud thread makes no progress for this long while the network
 * is up. The Memfault HTTP client's TLS connect() has no socket timeout, so a
 * network hiccup mid-handshake can park this thread forever — silently killing
 * heartbeat uploads and FOTA checks while everything else (Wi-Fi, audio, motor)
 * keeps running. Must comfortably exceed the longest legitimate blocking
 * operation (a full FOTA image download, ~3.5 min measured).
 */
#define CLOUD_STALL_REBOOT_S (15 * 60)
/* Reboot to recover if Wi-Fi stays down this long AFTER having been connected.
 * conn_mgr and the nRF70 backend retry association on their own, but a wedged
 * supplicant/driver (a marginal-signal drop that never re-associates - e.g. a
 * unit in a weak-signal spot) only clears on a reset. Armed only after the first
 * successful connect, so a unit simply out of range never reboot-loops: worst
 * case one reboot per outage, then it parks and the offline voice path runs. */
#define DISCONNECT_REBOOT_S  (10 * 60)
/* How often the stall monitor samples the progress timestamp. */
#define STALL_CHECK_PERIOD_S 60
/* Bounded retries for the one-shot network interface bring-up at startup. */
#define NET_BRINGUP_MAX_ATTEMPTS 5
#define NET_BRINGUP_RETRY_DELAY	 K_SECONDS(5)

/* Persisted total flush count: settings key "toilet/flush_count". */
#define FLUSH_COUNT_KEY	    "toilet/flush_count"

static K_SEM_DEFINE(network_ready_sem, 0, 1);
static K_SEM_DEFINE(date_time_ready_sem, 0, 1);
/* Given each time a flush occurs to wake the cloud thread for reporting. */
static K_SEM_DEFINE(flush_event_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
static atomic_t connected = ATOMIC_INIT(0);
/* Flushes that have happened but not yet been recorded. */
static atomic_t flush_pending = ATOMIC_INIT(0);
/* Total lifetime flush count, persisted in NVS via the settings subsystem. */
static uint32_t flush_count;
/* Uptime (seconds) of the cloud thread's last sign of life, fed at every loop
 * iteration. Read by the stall monitor timer.
 */
static atomic_t last_progress_s;
/* Uptime (seconds) when L4 connectivity was lost; 0 whenever connected. Read by
 * the monitor timer to time the disconnection watchdog. */
static atomic_t disconnected_since_s;
/* Set once the device has connected at least once this boot; gates the
 * disconnection watchdog so an environment with no Wi-Fi never reboot-loops. */
static atomic_t ever_connected = ATOMIC_INIT(0);

static void note_progress(void)
{
	atomic_set(&last_progress_s, (atomic_val_t)(k_uptime_get_32() / MSEC_PER_SEC));
}

/* Runs in timer (ISR) context, so it fires even if the cloud thread is wedged.
 * Guards two independent failure modes:
 *
 *  1. Connected but stuck: the cloud thread has made no progress for
 *     CLOUD_STALL_REBOOT_S while the link is up - almost certainly parked in an
 *     unbounded socket call (the Memfault TLS connect has no timeout). There is
 *     no safe way to kill a thread blocked in a syscall, so capture a coredump
 *     (its backtrace = where it parked) and reset via RESET_ON_FATAL_ERROR.
 *
 *  2. Disconnected too long: Wi-Fi has been down for DISCONNECT_REBOOT_S after
 *     having been up. conn_mgr / the nRF70 backend retry association themselves,
 *     but a wedged supplicant/driver only clears on a reset, so warm-reboot to
 *     force a clean bring-up. Reason-only (a lost link is not a crashed thread).
 *
 * Either way the voice/flush path keeps working offline and recovers ~16 s after
 * the reboot.
 */
static void stall_monitor_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	const uint32_t now_s = k_uptime_get_32() / MSEC_PER_SEC;

	if (atomic_get(&connected) == 1) {
		const uint32_t last_s = (uint32_t)atomic_get(&last_progress_s);

		if ((now_s - last_s) >= CLOUD_STALL_REBOOT_S) {
			MEMFAULT_SOFTWARE_WATCHDOG();
		}
		return;
	}

	/* Only reboot to recover a link we HAD and lost; a unit that never
	 * connected this boot is likely just out of range, and rebooting it would
	 * loop forever and starve the offline voice/flush path.
	 */
	if (atomic_get(&ever_connected) != 1) {
		return;
	}

	const uint32_t since_s = (uint32_t)atomic_get(&disconnected_since_s);

	if (since_s != 0 && (now_s - since_s) >= DISCONNECT_REBOOT_S) {
		/* Shares the SoftwareWatchdog reason with the cloud stall above; the
		 * two are distinguishable in Memfault by the connectivity state at
		 * reboot (ConnectionLost here vs. Connected for a cloud-thread stall).
		 */
		MEMFAULT_REBOOT_MARK_RESET_IMMINENT(kMfltRebootReason_SoftwareWatchdog);
		sys_reboot(SYS_REBOOT_WARM);
	}
}

static K_TIMER_DEFINE(cloud_stall_timer, stall_monitor_expiry, NULL);

bool cloud_is_connected(void)
{
	return atomic_get(&connected) == 1;
}

void cloud_report_flush(void)
{
	atomic_inc(&flush_pending);
	k_sem_give(&flush_event_sem);
}

static int toilet_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	if (settings_name_steq(name, "flush_count", NULL) && len == sizeof(flush_count)) {
		return read_cb(cb_arg, &flush_count, sizeof(flush_count)) < 0 ? -EIO : 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(toilet, "toilet", NULL, toilet_settings_set, NULL, NULL);

/* Publish the lifetime flush count as a Memfault metric so it appears in the
 * device timeline. Called every tick so each heartbeat carries the latest
 * value even in intervals with no flush.
 */
static void publish_flush_count(void)
{
	memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(flush_count), flush_count);
}

/* Record any pending flush events: bump the lifetime count, persist it to NVS,
 * and refresh the Memfault metric. Runs only on the cloud thread.
 */
static void record_flushes(void)
{
	while (atomic_get(&flush_pending) > 0) {
		atomic_dec(&flush_pending);
		flush_count++;

		int err = settings_save_one(FLUSH_COUNT_KEY, &flush_count, sizeof(flush_count));

		if (err) {
			LOG_WRN("Failed to persist flush count %u (err %d); will retry next flush",
				flush_count, err);
		}
		LOG_INF("Flush #%u recorded", flush_count);
	}

	publish_flush_count();
}

/* Drain pending Memfault data (reboot events, metrics, coredumps) to Memfault
 * over HTTPS. Bounded internally by the Memfault HTTP port.
 */
static void upload_memfault_data(void)
{
	if (!memfault_packetizer_data_available()) {
		return;
	}

	int err = memfault_zephyr_port_post_data();

	if (err) {
		LOG_WRN("Memfault data post failed (err %d)", err);
	}
}

/* Ask Memfault whether a newer release is deployed to this device's cohort. If
 * so, memfault_zephyr_fota_start() downloads it (HTTPS) into the MCUboot
 * secondary slot in external flash and reboots to apply it on success, so this
 * only returns when there is no update (0) or on error (<0).
 */
static void check_fota(void)
{
	int err = memfault_zephyr_fota_start();

	if (err < 0) {
		LOG_WRN("Memfault FOTA check failed (err %d)", err);
	}
}

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Network connectivity gained");
		/* Fresh progress baseline before the stall monitor arms. */
		note_progress();
		/* Arm the disconnection watchdog and clear any pending down-timer. */
		atomic_set(&ever_connected, 1);
		atomic_set(&disconnected_since_s, 0);
		atomic_set(&connected, 1);
		memfault_metrics_connectivity_connected_state_change(
			kMemfaultMetricsConnectivityState_Connected);
		k_sem_give(&network_ready_sem);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_INF("Network connectivity lost");
		/* Stamp the down-time before clearing `connected` so the monitor has a
		 * valid start point (the != 0 guard covers the transient in between).
		 */
		atomic_set(&disconnected_since_s,
			   (atomic_val_t)(k_uptime_get_32() / MSEC_PER_SEC));
		atomic_set(&connected, 0);
		memfault_metrics_connectivity_connected_state_change(
			kMemfaultMetricsConnectivityState_ConnectionLost);
		break;
	default:
		break;
	}
}

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	if (evt->type != DATE_TIME_NOT_OBTAINED) {
		k_sem_give(&date_time_ready_sem);
	}
}

static void cloud_thread_fn(void)
{
	/* Restore the persisted lifetime flush count from NVS. Best-effort: a
	 * failure here only means the flush counter starts at 0, not a fatal
	 * condition, so log and continue.
	 */
	int err = settings_subsys_init();

	if (err) {
		LOG_WRN("Settings subsystem init failed (err %d); flush count may not "
			"restore", err);
	} else {
		err = settings_load_subtree("toilet");
		if (err) {
			LOG_WRN("Failed to load persisted flush count (err %d)", err);
		}
	}
	LOG_INF("Flush count restored: %u", flush_count);

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
	date_time_register_handler(date_time_evt_handler);

	/* Bring all interfaces up and request connectivity (Wi-Fi uses the
	 * credentials stored via the `wifi cred` shell / static config). This is
	 * one-shot startup: if either call fails outright, the thread would
	 * otherwise wait on network_ready_sem forever with `connected` never set,
	 * which the stall monitor cannot catch (it only fires while connected).
	 *
	 * Retry a bounded number of times (transient boot-order races resolve
	 * quickly) rather than rebooting: a deterministic failure (missing Wi-Fi
	 * firmware, bad config) would otherwise reboot forever, starving the
	 * audio/flush path that must keep working even with no network at all.
	 * If it never comes up, log loudly and fall through - the thread parks
	 * on network_ready_sem, same as an unrecoverable link loss, and voice/
	 * flush continue unaffected.
	 */
	bool net_up = false;

	for (int attempt = 1; attempt <= NET_BRINGUP_MAX_ATTEMPTS; attempt++) {
		if (conn_mgr_all_if_up(true) == 0 && conn_mgr_all_if_connect(true) == 0) {
			net_up = true;
			break;
		}
		LOG_WRN("Failed to bring up network interfaces (attempt %d/%d)", attempt,
			NET_BRINGUP_MAX_ATTEMPTS);
		k_sleep(NET_BRINGUP_RETRY_DELAY);
	}

	if (!net_up) {
		LOG_ERR("Network interfaces never came up after %d attempts; cloud "
			"connectivity unavailable this boot", NET_BRINGUP_MAX_ATTEMPTS);
	}
	memfault_metrics_connectivity_connected_state_change(
		kMemfaultMetricsConnectivityState_Started);

	note_progress();
	k_timer_start(&cloud_stall_timer, K_SECONDS(STALL_CHECK_PERIOD_S),
		      K_SECONDS(STALL_CHECK_PERIOD_S));

	while (true) {
		LOG_INF("Waiting for network...");
		k_sem_take(&network_ready_sem, K_FOREVER);

		/* TLS to Memfault needs a valid wall clock for certificate
		 * date checks; retry NTP until it lands (or the link drops).
		 */
		while (atomic_get(&connected) == 1) {
			/* NTP retries are alive-and-logging, not a silent stall. */
			note_progress();
			LOG_INF("Obtaining date/time over NTP...");
			(void)date_time_update_async(date_time_evt_handler);
			if (k_sem_take(&date_time_ready_sem, K_SECONDS(DATE_TIME_TIMEOUT_S)) == 0) {
				break;
			}
			LOG_WRN("Failed to obtain date/time, retrying");
		}
		if (atomic_get(&connected) != 1) {
			continue;
		}

		LOG_INF("Network ready; uploading to Memfault over HTTPS");

		/* Reaching the network (Wi-Fi + valid time for TLS) proves this
		 * image boots and connects, so confirm it with MCUboot. The
		 * bootloader runs a freshly-swapped image in test mode
		 * (CONFIG_BOOT_SWAP_USING_MOVE) and reverts to the previous image
		 * on the next reset unless it is confirmed; without this a FOTA
		 * update is undone by the first power-cycle.
		 */
		if (!boot_is_img_confirmed()) {
			int cerr = boot_write_img_confirmed();

			if (cerr) {
				LOG_ERR("Failed to confirm running image (err %d)", cerr);
			} else {
				LOG_INF("Running image confirmed (FOTA update made permanent)");
			}
		}

		/* Push any data captured before/at this connection (e.g. the
		 * reboot reason event) promptly, and seed the flush metric.
		 */
		publish_flush_count();
		upload_memfault_data();

		int64_t next_fota_check = 0; /* check shortly after coming up */

		while (atomic_get(&connected) == 1) {
			note_progress();
			if (k_sem_take(&flush_event_sem, K_SECONDS(UPLOAD_TICK_S)) == 0) {
				record_flushes();
			} else {
				publish_flush_count();
				upload_memfault_data();
				if (k_uptime_get() >= next_fota_check) {
					check_fota();
					next_fota_check = k_uptime_get() +
							  (int64_t)FOTA_CHECK_S * MSEC_PER_SEC;
				}
			}
		}

		LOG_INF("Disconnected; will re-establish when network returns");
	}
}

/* Stack covers the Memfault HTTPS client (TLS handshake + chunk POST) and, when
 * an update is queued, FOTA job parsing; the image download itself runs on the
 * downloader's own thread (CONFIG_DOWNLOADER_STACK_SIZE).
 */
K_THREAD_DEFINE(cloud_tid, 8192, cloud_thread_fn, NULL, NULL, NULL, 7, 0, 0);
