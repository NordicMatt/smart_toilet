#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ww_kws)

# Compile in the nRF Cloud credentials (CA + device cert/key) when provisioning
# certificates at build time (secure build, no TF-M Protected Storage).
zephyr_include_directories_ifdef(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES certs)

# nrf_cloud_coap.h transitively includes nrf_cloud_pgps.h, which unconditionally
# includes nrf_cloud_agnss_schema_v1.h (in the nrf_cloud lib's common/include).
# Make that dir visible to app sources that include nrf_cloud_coap.h.

target_sources(app PRIVATE
  src/main.c

  src/actuator.c
  src/cloud.c
  src/control_output.c
  src/dmic.c
  src/leds.c

  src/ww/wakeword.c
  src/ww/nrf_edgeai_generated/nrf_edgeai_user_model.c

  src/kws/kws.c
  src/kws/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

if(CONFIG_NRF_CLOUD_COAP)
  target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/../nrf/subsys/net/lib/nrf_cloud/common/include)
endif()
