/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Memfault user platform config (included via CONFIG_MEMFAULT_USER_CONFIG_ENABLE).
 *
 * Point the HTTP client at Memfault's CURRENT ingress endpoints. The NCS port
 * defaults to chunks-nrf.memfault.com / device-nrf.memfault.com, which the nRF
 * Cloud FQDN reference lists as DEPRECATED (legacy nRF9160 / SDK <= 1.7.0). On
 * 2026-07-10 (~00:23 UTC) that legacy ingress was migrated (new LB, DigiCert
 * cert) and stopped serving our TLS clients -- the server answered every mbedTLS
 * ClientHello with an immediate close_notify, taking the whole fleet offline on
 * ALL firmware versions. The non-deprecated hosts are the fix.
 */

#pragma once

#define MEMFAULT_HTTP_CHUNKS_API_HOST "chunks.memfault.com"
#define MEMFAULT_HTTP_DEVICE_API_HOST "device.memfault.com"
