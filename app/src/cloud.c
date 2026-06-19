/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * nRF Cloud (CoAP over Wi-Fi) connectivity for the smart toilet.
 *
 * Runs in its own thread so the wake-word/DMIC loop in main.c is untouched:
 *   conn_mgr brings up Wi-Fi (using stored credentials) -> wait for L4 ->
 *   obtain time via NTP -> nrf_cloud_coap_connect(). Reconnects on drop.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <cJSON.h>
#include <date_time.h>
#include <net/nrf_cloud_coap.h>
#include <nrf_cloud_coap_transport.h>
#include <net/fota_download.h>
#include <net/nrf_cloud_fota_poll.h>
#include <zephyr/sys/reboot.h>

#include <memfault/core/data_packetizer.h>

#include "actuator.h"
#include "cloud.h"

LOG_MODULE_REGISTER(cloud, LOG_LEVEL_INF);

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define DATE_TIME_TIMEOUT_S 30
#define RECONNECT_DELAY_S   30
/* How often, while idle and connected, to poll the shadow for a remote-flush
 * command. CoAP is client-driven, so remote control is poll-based.
 */
#define REMOTE_POLL_S	    15
/* How often, while connected, to ask nRF Cloud whether a FOTA job is queued. */
#define FOTA_CHECK_S	    120

/* nRF Cloud appId for the flush telemetry messages. */
#define FLUSH_APP_ID	    "FLUSH"
/* Persisted total flush count: settings key "toilet/flush_count". */
#define FLUSH_COUNT_KEY	    "toilet/flush_count"

static K_SEM_DEFINE(network_ready_sem, 0, 1);
static K_SEM_DEFINE(date_time_ready_sem, 0, 1);
/* Given each time a flush occurs to wake the cloud thread for reporting. */
static K_SEM_DEFINE(flush_event_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
static atomic_t connected = ATOMIC_INIT(0);
/* Flushes that have happened but not yet been reported to the cloud. */
static atomic_t flush_pending = ATOMIC_INIT(0);
/* Total lifetime flush count, persisted in NVS via the settings subsystem. */
static uint32_t flush_count;

static void fota_reboot(enum nrf_cloud_fota_reboot_status status);
/* FOTA polling context; the reboot handler applies the update on the next boot. */
static struct nrf_cloud_fota_poll_ctx fota_ctx = {
	.reboot_fn = fota_reboot,
};
/* Next uptime (ms) at which to poll nRF Cloud for a FOTA job. */
static int64_t next_fota_check;

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

/* Report any pending flush events (one device message each) and push the
 * latest flush count to the device shadow. Runs only on the cloud thread.
 */
static void report_flushes(void)
{
	while (atomic_get(&flush_pending) > 0) {
		atomic_dec(&flush_pending);
		flush_count++;
		(void)settings_save_one(FLUSH_COUNT_KEY, &flush_count, sizeof(flush_count));

		int64_t ts = 0;
		(void)date_time_now(&ts);

		int err = nrf_cloud_coap_sensor_send(FLUSH_APP_ID, (double)flush_count,
						     ts > 0 ? ts : 0, false);
		if (err) {
			LOG_WRN("Flush event send failed (err %d)", err);
		} else {
			LOG_INF("Reported flush #%u to nRF Cloud", flush_count);
		}
	}

	char json[40];

	(void)snprintk(json, sizeof(json), "{\"flushCount\":%u}", flush_count);

	int err = nrf_cloud_coap_shadow_state_update(json);

	if (err) {
		LOG_WRN("Shadow flushCount update failed (err %d)", err);
	}
}

/* Poll the shadow delta for a remote flush command: {"flush": true}. On
 * receipt, trigger a flush and reconcile the shadow so the delta clears.
 */
static void poll_remote_flush(void)
{
	static char buf[256];
	size_t len = sizeof(buf);

	int err = nrf_cloud_coap_shadow_get(buf, &len, true, COAP_CONTENT_FORMAT_APP_JSON);

	if (err || len == 0) {
		return;
	}

	buf[MIN(len, sizeof(buf) - 1)] = '\0';

	cJSON *root = cJSON_Parse(buf);

	if (!root) {
		return;
	}

	/* The flush flag may arrive at the top level or nested under "state". */
	cJSON *scope = cJSON_GetObjectItem(root, "state");

	cJSON *flush = cJSON_GetObjectItem(scope ? scope : root, "flush");

	if (cJSON_IsTrue(flush)) {
		LOG_INF("Remote flush command received from nRF Cloud");
		actuator_flush();
		/* Match reported to desired so the delta is cleared. Toggle the
		 * desired flag in the portal to issue another remote flush.
		 */
		(void)nrf_cloud_coap_shadow_state_update("{\"flush\":true}");
	}

	cJSON_Delete(root);
}

/* CoAP response callback for Memfault chunk uploads (fire-and-forget). */
static void memfault_post_cb(const struct coap_client_response_data *data, void *user)
{
	ARG_UNUSED(user);

	if (data->result_code < 0) {
		LOG_WRN("Memfault chunk upload error: %d", data->result_code);
	} else if (data->result_code >= COAP_RESPONSE_CODE_BAD_REQUEST) {
		LOG_WRN("Memfault chunk upload rejected: %d.%02d", data->result_code >> 5,
			data->result_code & 0x1f);
	} else if (data->last_block) {
		LOG_INF("Memfault chunk uploaded (%d.%02d)", data->result_code >> 5,
			data->result_code & 0x1f);
	}
}

/* Forward pending Memfault data (reboot events, metrics, coredumps) to nRF
 * Cloud's "chunks" resource, which relays it to Memfault. Bounded per call so
 * a large coredump cannot monopolise the single CoAP client; the rest drains
 * on subsequent ticks.
 */
static void upload_memfault_chunks(void)
{
	static uint8_t chunk[512];

	for (int i = 0; i < 4 && memfault_packetizer_data_available(); i++) {
		size_t len = sizeof(chunk);

		if (!memfault_packetizer_get_chunk(chunk, &len)) {
			break;
		}

		int err = nrf_cloud_coap_post("chunks", NULL, chunk, len,
					      COAP_CONTENT_FORMAT_APP_OCTET_STREAM, true,
					      memfault_post_cb, NULL);
		if (err) {
			LOG_WRN("Memfault chunk post failed (err %d)", err);
			memfault_packetizer_abort();
			break;
		}
	}
}

/* Reboot to apply or finalize a FOTA update. The downloaded image lives in
 * the external-flash secondary slot; MCUboot swaps it into the internal
 * primary slot on the next boot.
 */
static void fota_reboot(enum nrf_cloud_fota_reboot_status status)
{
	LOG_INF("FOTA reboot requested (status %d); rebooting to apply update", status);
	k_sleep(K_SECONDS(1)); /* let the log line drain */
	sys_reboot(SYS_REBOOT_COLD);
}

/* Advertise application-FOTA support to nRF Cloud (shadow serviceInfo
 * "fota_v2"). Without this the portal will not offer application updates.
 */
static void advertise_fota_support(void)
{
	struct nrf_cloud_svc_info_fota fota = { .application = 1 };
	struct nrf_cloud_svc_info svc = { .fota = &fota };

	int err = nrf_cloud_coap_shadow_service_info_update(&svc);

	if (err) {
		LOG_WRN("FOTA service-info update failed (err %d)", err);
	}
}

/* Ask nRF Cloud whether a FOTA job is queued; if so, download it over the
 * CoAP connection and stage it. A staged image triggers a reboot via
 * fota_reboot(), so this returns only when there is no job (-EAGAIN) or on
 * error. Runs on the cloud thread, so it shares the single nRF Cloud CoAP
 * client without concurrent access.
 */
static void check_fota(void)
{
	int err = nrf_cloud_fota_poll_process(&fota_ctx);

	if (err == -EAGAIN) {
		return; /* no job queued */
	}
	if (err < 0) {
		LOG_WRN("FOTA poll failed (err %d)", err);
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
		k_sem_give(&network_ready_sem);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_INF("Network connectivity lost");
		atomic_set(&connected, 0);
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
	int err;

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init failed (err %d)", err);
		return;
	}

	/* Restore the persisted lifetime flush count from NVS. */
	(void)settings_subsys_init();
	(void)settings_load_subtree("toilet");
	LOG_INF("Flush count restored: %u", flush_count);

	/* FOTA polling assistance: downloads + stages images to the MCUboot
	 * secondary slot (in external flash). Non-fatal if it fails to init.
	 */
	err = nrf_cloud_fota_poll_init(&fota_ctx);
	if (err) {
		LOG_WRN("nrf_cloud_fota_poll_init failed (err %d); FOTA disabled", err);
	}

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
	date_time_register_handler(date_time_evt_handler);

	/* Bring all interfaces up and request connectivity (Wi-Fi uses the
	 * credentials stored via the `wifi cred` shell).
	 */
	(void)conn_mgr_all_if_up(true);
	(void)conn_mgr_all_if_connect(true);

	while (true) {
		LOG_INF("Waiting for network...");
		k_sem_take(&network_ready_sem, K_FOREVER);

		LOG_INF("Obtaining date/time over NTP...");
		(void)date_time_update_async(date_time_evt_handler);
		if (k_sem_take(&date_time_ready_sem, K_SECONDS(DATE_TIME_TIMEOUT_S)) != 0) {
			LOG_WRN("Failed to obtain date/time, retrying");
			continue;
		}

		LOG_INF("Connecting to nRF Cloud...");
		err = nrf_cloud_coap_connect(NULL);
		if (err) {
			LOG_ERR("nrf_cloud_coap_connect failed (err %d), retry in %ds",
				err, RECONNECT_DELAY_S);
			k_sleep(K_SECONDS(RECONNECT_DELAY_S));
			continue;
		}

		LOG_INF("Connected to nRF Cloud");
		atomic_set(&connected, 1);

		/* Finalize a FOTA job that completed just before a reboot (image
		 * validation), then tell the cloud we accept application updates.
		 */
		(void)nrf_cloud_fota_poll_process_pending(&fota_ctx);
		advertise_fota_support();
		next_fota_check = 0; /* check for a job shortly after connecting */

		/* Push the current flush count to the shadow on (re)connect, then
		 * service flush events as they occur and poll for remote-flush
		 * commands while idle.
		 */
		report_flushes();
		/* Push any data captured before/at this connection (e.g. the
		 * reboot reason event) to Memfault promptly.
		 */
		upload_memfault_chunks();

		while (atomic_get(&connected) == 1) {
			if (k_sem_take(&flush_event_sem, K_SECONDS(REMOTE_POLL_S)) == 0) {
				report_flushes();
			} else {
				poll_remote_flush();
				upload_memfault_chunks();
				if (k_uptime_get() >= next_fota_check) {
					check_fota();
					next_fota_check = k_uptime_get() +
							  (int64_t)FOTA_CHECK_S * MSEC_PER_SEC;
				}
			}
		}

		LOG_INF("Disconnected; will re-establish when network returns");
		(void)nrf_cloud_coap_disconnect();
	}
}

/* Runs the DTLS handshake + ECDSA JWT signing, the cJSON shadow/message
 * encoding, and (when a job is queued) FOTA job-document parsing and download
 * coordination. 8 KB covers the added FOTA work; the actual image download
 * runs on the downloader's own thread (CONFIG_DOWNLOADER_STACK_SIZE).
 */
K_THREAD_DEFINE(cloud_tid, 8192, cloud_thread_fn, NULL, NULL, NULL, 7, 0, 0);
