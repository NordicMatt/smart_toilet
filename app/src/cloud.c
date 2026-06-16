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
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>

#include <date_time.h>
#include <net/nrf_cloud_coap.h>

#include "cloud.h"

LOG_MODULE_REGISTER(cloud, LOG_LEVEL_INF);

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define DATE_TIME_TIMEOUT_S 30
#define RECONNECT_DELAY_S   30

static K_SEM_DEFINE(network_ready_sem, 0, 1);
static K_SEM_DEFINE(date_time_ready_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
static atomic_t connected = ATOMIC_INIT(0);

bool cloud_is_connected(void)
{
	return atomic_get(&connected) == 1;
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

		/* Stay connected. Flush reporting / shadow polling is added in
		 * step 2; for now just hold the connection open.
		 */
		while (atomic_get(&connected) == 1) {
			k_sleep(K_SECONDS(60));
		}

		LOG_INF("Disconnected; will re-establish when network returns");
		(void)nrf_cloud_coap_disconnect();
	}
}

/* The DTLS handshake (cert-chain verification + ECDSA JWT signing) is very
 * stack-hungry, so this thread needs a large stack.
 */
K_THREAD_DEFINE(cloud_tid, 16384, cloud_thread_fn, NULL, NULL, NULL, 7, 0, 0);
