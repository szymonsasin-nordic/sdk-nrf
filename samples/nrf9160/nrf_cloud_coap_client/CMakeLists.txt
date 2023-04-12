#
# Copyright (c) 2022 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf-cloud-coap-client)
find_program(ZCBOR zcbor REQUIRED)

# NORDIC SDK APP START
target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_WIFI app PRIVATE src/scan_wifi.c)
# NORDIC SDK APP END

zephyr_include_directories(src)
