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
target_sources(app PRIVATE src/app_jwt.c)
target_sources(app PRIVATE src/coap_client.c)
target_sources(app PRIVATE src/coap_codec.c)
target_sources(app PRIVATE src/dtls.c)
target_sources(app PRIVATE src/nrf_cloud_coap.c)
target_sources(app PRIVATE src/ground_fix_encode.c)
target_sources(app PRIVATE src/ground_fix_decode.c)
target_sources(app PRIVATE src/agps_encode.c)
target_sources(app PRIVATE src/msg_encode.c)
# NORDIC SDK APP END
zephyr_compile_definitions(nrf-cloud-coap_client CDDL_CBOR_CANONICAL)

zephyr_include_directories(src)
