# Kconfig - nRF91 APRGW board configuration
#
# Copyright (c) 2019 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic

if(CONFIG_BOARD_NRF9160_APRGW AND NOT DEFINED CONFIG_MCUBOOT)
	zephyr_library()
	zephyr_library_sources(board_secure.c)
endif()

if(CONFIG_BOARD_NRF9160_APRGWNS)
	zephyr_library()
	zephyr_library_sources(board_nonsecure.c)
endif()

zephyr_library_sources(nrf52840_reset.c)