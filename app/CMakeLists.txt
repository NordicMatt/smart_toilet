#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ww_kws)

target_sources(app PRIVATE
  src/main.c

  src/actuator.c
  src/cloud.c
  src/control_output.c
  src/dmic.c
  src/leds.c

  src/ww/wakeword.c
)

# The keyword-spotting stage and its model (~26 KB RAM) are only built for the
# KWS modes. The smart toilet runs APP_MODE_WW_ONLY, so they are omitted to
# leave room for the Wi-Fi/cloud/Memfault stacks.
target_sources_ifndef(CONFIG_APP_MODE_WW_ONLY app PRIVATE
  src/kws/kws.c
  src/kws/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

# Compile in the nRF Cloud credentials when provisioning certs at build time.
zephyr_include_directories_ifdef(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES certs)

if(CONFIG_NRF_CLOUD_COAP)
  target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/../nrf/subsys/net/lib/nrf_cloud/common/include
    # nrf_cloud_coap_transport.h exposes nrf_cloud_coap_post(), used to forward
    # Memfault chunks to nRF Cloud's "chunks" resource over our CoAP connection.
    ${ZEPHYR_BASE}/../nrf/subsys/net/lib/nrf_cloud/coap/include)
endif()

# Wake word model selected via the APP_WW_MODEL Kconfig choice. Each model
# directory carries its own nrf_edgeai_generated/ files; the include
# directory makes wakeword.c's "nrf_edgeai_generated/..." include resolve
# to the selected model.
if(CONFIG_APP_WW_MODEL_OKAY_NORDIC)
  set(WW_MODEL_DIR src/ww/models/okay_nordic)
else()
  set(WW_MODEL_DIR src/ww/models/abracadabra)
endif()
target_sources(app PRIVATE ${WW_MODEL_DIR}/nrf_edgeai_generated/nrf_edgeai_user_model.c)
target_include_directories(app PRIVATE ${WW_MODEL_DIR})

target_sources_ifdef(CONFIG_MEMFAULT app PRIVATE src/audio_telemetry.c)
# Application Memfault heartbeat metric definitions
# (memfault_metrics_heartbeat_config.def lives in config/).
zephyr_include_directories_ifdef(CONFIG_MEMFAULT config)
target_sources_ifdef(CONFIG_APP_AUDIO_STATS app PRIVATE src/audio_stats.c)
target_sources_ifdef(CONFIG_APP_AUDIO_PROC app PRIVATE src/audio_proc.c)
target_sources_ifdef(CONFIG_APP_AUDIO_SNAP app PRIVATE src/audio_snap.c)
