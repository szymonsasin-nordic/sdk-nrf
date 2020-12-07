/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <zephyr.h>
#include <logging/log.h>
#include <dfu/mcuboot.h>
#include <dfu/dfu_target.h>
#include "dfu_target_uart.h"

#define DEF_DFU_TARGET(name) \
static const struct dfu_target dfu_target_ ## name  = { \
	.init = dfu_target_ ## name ## _init, \
	.offset_get = dfu_target_## name ##_offset_get, \
	.write = dfu_target_ ## name ## _write, \
	.done = dfu_target_ ## name ## _done, \
}

#ifdef CONFIG_DFU_TARGET_MODEM
#include "dfu_target_modem.h"
DEF_DFU_TARGET(modem);
#endif
#ifdef CONFIG_DFU_TARGET_MCUBOOT
#include "dfu_target_mcuboot.h"
DEF_DFU_TARGET(mcuboot);
#endif

#ifdef CONFIG_DFU_TARGET_UART
const struct dfu_target dfu_target_uart = {
	.init  = dfu_target_uart_init,
	.offset_get = dfu_target_uart_offset_get,
	.write = dfu_target_uart_write,
	.done  = dfu_target_uart_done,
};
#endif

#define MIN_SIZE_IDENTIFY_BUF 32

LOG_MODULE_REGISTER(dfu_target, CONFIG_DFU_TARGET_LOG_LEVEL);

static const struct dfu_target *current_target;

int dfu_target_img_type(const void *const buf, size_t len)
{
	#if defined(CONFIG_DFU_TARGET_MCUBOOT)
		LOG_INF("CONFIG_DFU_TARGET_MCUBOOT is defined");
		if(dfu_target_mcuboot_identify(buf)) {
			return DFU_TARGET_IMAGE_TYPE_MCUBOOT;
		}
	#endif
	#if defined(CONFIG_DFU_TARGET_MODEM)
		LOG_INF("CONFIG_DFU_TARGET_MODEM is defined");
		if(dfu_target_modem_identify(buf)) {
			return DFU_TARGET_IMAGE_TYPE_MODEM_DELTA;
		}
	#endif
	#if defined(CONFIG_DFU_TARGET_UART)
		if(dfu_target_uart_identify(buf)) {
			printk("Image header is equal to 0x85f3d83a and "
			       "the UART type is chosen as dfu target\n");
			return DFU_TARGET_IMAGE_TYPE_UART;
		}	
	#endif

	if (len < MIN_SIZE_IDENTIFY_BUF) {
		return -EAGAIN;
	}

	LOG_ERR("Received image type differs from expected image type");
	return -ENOTSUP;
}

int dfu_target_init(int img_type, size_t file_size, dfu_target_callback_t cb)
{
	const struct dfu_target *new_target = NULL;

	#if defined(CONFIG_DFU_TARGET_MCUBOOT)
		LOG_INF("CONFIG_DFU_TARGET_MCUBOOT is defined");
		if(img_type == DFU_TARGET_IMAGE_TYPE_MCUBOOT){
			new_target = &dfu_target_mcuboot;
		}
	#elif defined(CONFIG_DFU_TARGET_MODEM)
		LOG_INF("CONFIG_DFU_TARGET_MODEM is defined");
		if(img_type == DFU_TARGET_IMAGE_TYPE_MODEM_DELTA){
			new_target = &dfu_target_modem;
		}
	#elif defined(CONFIG_DFU_TARGET_UART)
		LOG_INF("CONFIG_DFU_TARGET_UART is defined");
		if(img_type == DFU_TARGET_IMAGE_TYPE_UART){
			new_target = &dfu_target_uart;
		}
	#endif

	if (new_target == NULL) {
		LOG_ERR("Unknown image type");
		return -ENOTSUP;
	}

	/* The user is re-initializing with an previously aborted target.
	 * Avoid re-initializing generally to ensure that the download can
	 * continue where it left off. Re-initializing is required for modem
	 * upgrades to re-open the DFU socket that is closed on abort.
	 */
	if (new_target == current_target
	   && img_type != DFU_TARGET_IMAGE_TYPE_MODEM_DELTA) {
		return 0;
	}

	current_target = new_target;

	return current_target->init(file_size, cb);
}

int dfu_target_offset_get(size_t *offset)
{
	if (current_target == NULL) {
		return -EACCES;
	}

	return current_target->offset_get(offset);
}

int dfu_target_write(const void *const buf, size_t len)
{
	if (current_target == NULL || buf == NULL) {
		return -EACCES;
	}

	return current_target->write(buf, len);
}

int dfu_target_done(bool successful)
{
	int err;

	if (current_target == NULL) {
		return -EACCES;
	}

	err = current_target->done(successful);
	if (err != 0) {
		LOG_ERR("Unable to clean up dfu_target");
		return err;
	}

	if (successful) {
		current_target = NULL;
	}

	return 0;
}

int dfu_target_reset(void)
{
	if (current_target != NULL) {
		int err = current_target->done(false);

		if (err != 0) {
			LOG_ERR("Unable to clean up dfu_target");
			return err;
		}
	}
	current_target = NULL;
	return 0;
}
