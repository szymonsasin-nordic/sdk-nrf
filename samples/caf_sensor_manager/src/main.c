/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <app_event_manager.h>

#define MODULE main

#include <caf/events/module_state_event.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(MODULE);

#include "event_proxy_init.h"

void main(void)
{
	int ret;

	ret = app_event_manager_init();
	if (ret) {
		LOG_ERR("Event Manager not initialized, err: %d", ret);
		__ASSERT_NO_MSG(false);
		return;
	}
	LOG_INF("Event manager initialized");

	ret = init_event_proxy("sensor_data_aggregator_event");
	if (ret) {
		LOG_ERR("Event Manager Proxy not initialized, err: %d", ret);
		__ASSERT_NO_MSG(false);
		return;
	}

	module_set_state(MODULE_STATE_READY);
}
