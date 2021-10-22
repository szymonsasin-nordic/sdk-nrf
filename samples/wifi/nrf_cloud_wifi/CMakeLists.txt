# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})
project(nrf_cloud_wifi)

target_sources(app PRIVATE src/main.c src/libc_support.c)

if(CONFIG_EXTERNAL_LIBC)
				message("external libc")
				zephyr_include_directories(
								$ENV{ESPRESSIF_TOOLCHAIN_PATH}/xtensa-esp32-elf/include
								$ENV{ESPRESSIF_TOOLCHAIN_PATH}/xtensa-esp32-elf/sys-include
							)
				target_link_options(
								zephyr_interface BEFORE INTERFACE
								-L$ENV{ESPRESSIF_TOOLCHAIN_PATH}/xtensa-esp32-elf/lib
								)

				zephyr_link_libraries(
								c nosys
								)
				zephyr_compile_options(
								--specs=nosys.specs
								)
else()
				message("zephyr minimal libc")
				zephyr_library_sources_ifdef(
					CONFIG_SOC_ESP32
					src/scanf.c
					src/atob.c
					src/strtod.c
				)
endif()

