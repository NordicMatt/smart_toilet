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
  src/control_output.c
  src/dmic.c
  src/leds.c

  src/ww/wakeword.c

  src/kws/kws.c
  src/kws/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

# Wake word model selected via the APP_WW_MODEL Kconfig choice. Each model
# directory carries its own nrf_edgeai_generated/ files; the include
# directory makes wakeword.c's "nrf_edgeai_generated/..." include resolve
# to the selected model.
if(CONFIG_APP_WW_MODEL_SHAZAAM)
  set(WW_MODEL_DIR src/ww/models/shazaam)
else()
  set(WW_MODEL_DIR src/ww/models/okay_nordic)
endif()
target_sources(app PRIVATE ${WW_MODEL_DIR}/nrf_edgeai_generated/nrf_edgeai_user_model.c)
target_include_directories(app PRIVATE ${WW_MODEL_DIR})

target_sources_ifdef(CONFIG_APP_AUDIO_STATS app PRIVATE src/audio_stats.c)
target_sources_ifdef(CONFIG_APP_AUDIO_PROC app PRIVATE src/audio_proc.c)
target_sources_ifdef(CONFIG_APP_AUDIO_SNAP app PRIVATE src/audio_snap.c)
