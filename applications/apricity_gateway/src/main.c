/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <kernel_structs.h>
#include <stdio.h>
#include <string.h>
#include <drivers/gps.h>
#include <drivers/sensor.h>
#include <console/console.h>
#include <power/reboot.h>
#include <logging/log_ctrl.h>
#if defined(CONFIG_BSD_LIBRARY)
#include <net/bsdlib.h>
#include <bsd.h>
#include <lte_lc.h>
//#include <net/modem_info.h>
#endif /* CONFIG_BSD_LIBRARY */
#include <net/cloud.h>
#include <net/socket.h>
#include <nrf_cloud.h>
#if defined(CONFIG_NRF_CLOUD_AGPS)
#include <net/nrf_cloud_agps.h>
#endif

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <dfu/mcuboot.h>
#endif

#include "ui.h"
#include "watchdog.h"
#include "ble_conn_mgr.h"
#include "ble.h"


#include <logging/log.h>
#if !defined(CONFIG_NRF91_SOCKET_SEND_SPLIT_LARGE_BLOCKS)
LOG_MODULE_REGISTER(apricity_gateway_certs, CONFIG_APRICITY_GATEWAY_LOG_LEVEL);
#else
LOG_MODULE_REGISTER(apricity_gateway, CONFIG_APRICITY_GATEWAY_LOG_LEVEL);
#endif

#define CALIBRATION_PRESS_DURATION 	K_SECONDS(5)
#define CLOUD_CONNACK_WAIT_DURATION	K_SECONDS(CONFIG_CLOUD_WAIT_DURATION)

#if defined(CONFIG_BSD_LIBRARY) && \
!defined(CONFIG_LTE_LINK_CONTROL)
#error "Missing CONFIG_LTE_LINK_CONTROL"
#endif
#if defined(CONFIG_BSD_LIBRARY) && \
defined(CONFIG_LTE_AUTO_INIT_AND_CONNECT) && \
defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES)
#error "PROVISION_CERTIFICATES \
	requires CONFIG_LTE_AUTO_INIT_AND_CONNECT to be disabled!"
#endif

#define CLOUD_LED_ON_STR "{\"led\":\"on\"}"
#define CLOUD_LED_OFF_STR "{\"led\":\"off\"}"
#define CLOUD_LED_MSK UI_LED_1

/* Interval in milliseconds after which the device will reboot
 * if the disconnect event has not been handled.
 */
#define REBOOT_AFTER_DISCONNECT_WAIT_MS	K_SECONDS(15)

/* Interval in milliseconds after which the device will
 * disconnect and reconnect if association was not completed.
 */
#define CONN_CYCLE_AFTER_ASSOCIATION_REQ_MS	K_MINUTES(5)

struct rsrp_data {
	u16_t value;
	u16_t offset;
};

/* Stack definition for application workqueue */
K_THREAD_STACK_DEFINE(application_stack_area,
		      CONFIG_APPLICATION_WORKQUEUE_STACK_SIZE);
static struct k_work_q application_work_q;
static struct cloud_backend *cloud_backend;

static atomic_t cloud_connect_attempts;


#if IS_ENABLED(CONFIG_GPS_START_ON_MOTION)
/* Current state of activity monitor */
static motion_activity_state_t last_activity_state = MOTION_ACTIVITY_NOT_KNOWN;
#endif

/* Variable to keep track of nRF cloud association state. */
enum cloud_association_state {
	CLOUD_ASSOCIATION_STATE_INIT,
	CLOUD_ASSOCIATION_STATE_REQUESTED,
	CLOUD_ASSOCIATION_STATE_PAIRED,
	CLOUD_ASSOCIATION_STATE_RECONNECT,
	CLOUD_ASSOCIATION_STATE_READY,
};
static atomic_val_t cloud_association =
	ATOMIC_INIT(CLOUD_ASSOCIATION_STATE_INIT);


/* Structures for work */
static struct k_delayed_work cloud_reboot_work;
static struct k_delayed_work cycle_cloud_connection_work;
static struct k_delayed_work cloud_connect_work;
static struct k_work device_status_work;


#if defined(CONFIG_AT_CMD)
#define MODEM_AT_CMD_BUFFER_LEN (3000 + 1)
#else
#define MODEM_AT_CMD_NOT_ENABLED_STR "Error: AT Command driver is not enabled"
#define MODEM_AT_CMD_BUFFER_LEN (sizeof(MODEM_AT_CMD_NOT_ENABLED_STR))
#endif
#define MODEM_AT_CMD_RESP_TOO_BIG_STR "Error: AT Command response is too large to be sent"
#define MODEM_AT_CMD_MAX_RESPONSE_LEN (3000)

static K_SEM_DEFINE(modem_at_cmd_sem, 1, 1);


enum error_type {
	ERROR_CLOUD,
	ERROR_BSD_RECOVERABLE,
	ERROR_LTE_LC,
	ERROR_SYSTEM_FAULT
};

/* Forward declaration of functions */

static void work_init(void);
static void cycle_cloud_connection(struct k_work *work);
static void connection_evt_handler(const struct cloud_event *const evt);

static void shutdown_modem(void)
{
//#if defined(CONFIG_LTE_LINK_CONTROL)
//	/* Turn off and shutdown modem */
//	LOG_ERR("LTE link disconnect");
//	int err = lte_lc_power_off();
//
//	if (err) {
//		LOG_ERR("lte_lc_power_off failed: %d", err);
//	}
//#endif /* CONFIG_LTE_LINK_CONTROL */
//#if defined(CONFIG_BSD_LIBRARY)
//	LOG_ERR("Shutdown modem");
//	bsdlib_shutdown();
//#endif
}

/**@brief nRF Cloud error handler. */
void error_handler(enum error_type err_type, int err_code)
{
	atomic_set(&cloud_association, CLOUD_ASSOCIATION_STATE_INIT);

	if (err_type == ERROR_CLOUD) {
		shutdown_modem();
                printk("ERROR SHUTDOWN");
	}

#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
	LOG_PANIC();
	sys_reboot(0);
#else
	switch (err_type) {
	case ERROR_CLOUD:
		/* Blinking all LEDs ON/OFF in pairs (1 and 4, 2 and 3)
		 * if there is an application error.
		 */
		ui_led_set_pattern(UI_LED_ERROR_CLOUD);
		LOG_ERR("Error of type ERROR_CLOUD: %d", err_code);
	break;
	case ERROR_BSD_RECOVERABLE:
		/* Blinking all LEDs ON/OFF in pairs (1 and 3, 2 and 4)
		 * if there is a recoverable error.
		 */
		ui_led_set_pattern(UI_LED_ERROR_BSD_REC);
		LOG_ERR("Error of type ERROR_BSD_RECOVERABLE: %d", err_code);
	break;
	default:
		/* Blinking all LEDs ON/OFF in pairs (1 and 2, 3 and 4)
		 * undefined error.
		 */
		ui_led_set_pattern(UI_LED_ERROR_UNKNOWN);
		LOG_ERR("Unknown error type: %d, code: %d",
			err_type, err_code);
	break;
	}

	while (true) {
		k_cpu_idle();
	}
#endif /* CONFIG_DEBUG */
}

void cloud_error_handler(int err)
{
	error_handler(ERROR_CLOUD, err);
}

void cloud_connect_error_handler(enum cloud_connect_result err)
{
	bool reboot = true;
	char *backend_name = "invalid";

	if (err == CLOUD_CONNECT_RES_SUCCESS) {
		return;
	}

	LOG_ERR("Failed to connect to cloud, error %d", err);

	switch (err) {
	case CLOUD_CONNECT_RES_ERR_NOT_INITD: {
		LOG_ERR("Cloud back-end has not been initialized");
		/* no need to reboot, program error */
		reboot = false;
		break;
	}
	case CLOUD_CONNECT_RES_ERR_NETWORK: {
		LOG_ERR("Network error, check cloud configuration");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_BACKEND: {
		if (cloud_backend && cloud_backend->config &&
		    cloud_backend->config->name) {
			backend_name = cloud_backend->config->name;
		}
		LOG_ERR("An error occurred specific to the cloud back-end: %s",
			backend_name);
		break;
	}
	case CLOUD_CONNECT_RES_ERR_PRV_KEY: {
		LOG_ERR("Ensure device has a valid private key");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_CERT: {
		LOG_ERR("Ensure device has a valid CA and client certificate");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_CERT_MISC: {
		LOG_ERR("A certificate/authorization error has occurred");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA: {
		LOG_ERR("Connect timeout. SIM card may be out of data");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED: {
		LOG_ERR("Connection already exists.");
		break;
	}
	case CLOUD_CONNECT_RES_ERR_MISC: {
		break;
	}
	default: {
		LOG_ERR("Unhandled connect error");
		break;
	}
	}

	if (reboot) {
		LOG_ERR("Device will reboot in %d seconds",
				CONFIG_CLOUD_CONNECT_ERR_REBOOT_S);
		k_delayed_work_submit_to_queue(
			&application_work_q, &cloud_reboot_work,
			K_SECONDS(CONFIG_CLOUD_CONNECT_ERR_REBOOT_S));
	}

	ui_led_set_pattern(UI_LED_ERROR_CLOUD);
	shutdown_modem();
	k_thread_suspend(k_current_get());
}

/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
	error_handler(ERROR_BSD_RECOVERABLE, (int)err);
}

void connect_to_cloud(const s32_t connect_delay_s)
{
	static bool initial_connect = true;

	/* Ensure no data can be sent to cloud before connection is established.
	 */
	atomic_set(&cloud_association, CLOUD_ASSOCIATION_STATE_INIT);
	atomic_inc(&cloud_connect_attempts);

	/* Check if max cloud connect retry count is exceeded. */
	if (atomic_get(&cloud_connect_attempts) >
	    CONFIG_CLOUD_CONNECT_COUNT_MAX) {
		LOG_ERR("The max cloud connection attempt count exceeded.");
		cloud_error_handler(-ETIMEDOUT);
	}

	if (!initial_connect) {
		LOG_INF("Attempting reconnect in %d seconds...",
			connect_delay_s);
		k_delayed_work_cancel(&cloud_reboot_work);
	} else {
		initial_connect =  false;
	}

	k_delayed_work_submit_to_queue(&application_work_q,
				       &cloud_connect_work,
				       K_SECONDS(connect_delay_s));
}

static void cloud_connect_work_fn(struct k_work *work)
{
	int ret;

	LOG_INF("Connecting to cloud, attempt %d of %d",
	       atomic_get(&cloud_connect_attempts),
		   CONFIG_CLOUD_CONNECT_COUNT_MAX);

	k_delayed_work_submit_to_queue(&application_work_q,
			&cloud_reboot_work,
			CLOUD_CONNACK_WAIT_DURATION);

	ui_led_set_pattern(UI_CLOUD_CONNECTING);

	/* Attempt cloud connection */
	ret = cloud_connect(cloud_backend);
	if (ret != CLOUD_CONNECT_RES_SUCCESS) {
		k_delayed_work_cancel(&cloud_reboot_work);
		/* Will not return from this function.
		 * If the connect fails here, it is likely
		 * that user intervention is required.
		 */
		cloud_connect_error_handler(ret);
	} else {
		LOG_INF("Cloud connection request sent.");
		LOG_INF("Connection response timeout is set to %d seconds.",
		       CLOUD_CONNACK_WAIT_DURATION / MSEC_PER_SEC);
//		k_delayed_work_submit_to_queue(&application_work_q,
//					       &cloud_reboot_work,
//					       CLOUD_CONNACK_WAIT_DURATION);
	}
}

/**@brief Reboot the device if CONNACK has not arrived. */
static void cloud_reboot_handler(struct k_work *work)
{
	error_handler(ERROR_CLOUD, -ETIMEDOUT);
        printk("Cloud Error: Device Reboot\n");
}

/**@brief nRF Cloud specific callback for cloud association event. */
static void on_user_pairing_req(const struct cloud_event *evt)
{
	if (atomic_get(&cloud_association) !=
		CLOUD_ASSOCIATION_STATE_REQUESTED) {
		atomic_set(&cloud_association,
				   CLOUD_ASSOCIATION_STATE_REQUESTED);
		ui_led_set_pattern(UI_CLOUD_PAIRING);
		LOG_INF("Add device to cloud account.");
		LOG_INF("Waiting for cloud association...");

		/* If the association is not done soon enough (< ~5 min?)
		 * a connection cycle is needed... TBD why.
		 */
		k_delayed_work_submit_to_queue(&application_work_q,
					       &cycle_cloud_connection_work,
					       CONN_CYCLE_AFTER_ASSOCIATION_REQ_MS);
	}
}

static void cycle_cloud_connection(struct k_work *work)
{
	s32_t reboot_wait_ms = REBOOT_AFTER_DISCONNECT_WAIT_MS;

	LOG_INF("Disconnecting from cloud...");

	if (cloud_disconnect(cloud_backend) != 0) {
		reboot_wait_ms = K_SECONDS(5);
		LOG_INF("Disconnect failed. Device will reboot in %d seconds",
			(reboot_wait_ms / MSEC_PER_SEC));
	}

	/* Reboot fail-safe on disconnect */
	k_delayed_work_submit_to_queue(&application_work_q, &cloud_reboot_work,
				       reboot_wait_ms);
}

/** @brief Handle procedures after successful association with nRF Cloud. */
void on_pairing_done(void)
{
	if (atomic_get(&cloud_association) ==
			CLOUD_ASSOCIATION_STATE_REQUESTED) {
		k_delayed_work_cancel(&cycle_cloud_connection_work);

		/* After successful association, the device must
		 * reconnect to the cloud.
		 */
		LOG_INF("Device associated with cloud.");
		LOG_INF("Reconnecting for cloud policy to take effect.");
		atomic_set(&cloud_association,
				   CLOUD_ASSOCIATION_STATE_RECONNECT);
		k_delayed_work_submit_to_queue(&application_work_q,
					       &cycle_cloud_connection_work,
					       K_NO_WAIT);
	} else {
		atomic_set(&cloud_association, CLOUD_ASSOCIATION_STATE_PAIRED);
	}
}

void cloud_event_handler(const struct cloud_backend *const backend,
			 const struct cloud_event *const evt,
			 void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case CLOUD_EVT_CONNECTED:
	case CLOUD_EVT_CONNECTING:
	case CLOUD_EVT_DISCONNECTED:
		connection_evt_handler(evt);
		break;
	case CLOUD_EVT_READY:
		LOG_INF("CLOUD_EVT_READY");
		ui_led_set_pattern(UI_CLOUD_CONNECTED);
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		/* Mark image as good to avoid rolling back after update */
		boot_write_img_confirmed();
#endif
		atomic_set(&cloud_association, CLOUD_ASSOCIATION_STATE_READY);
//		sensors_start();
		break;
	case CLOUD_EVT_ERROR:
		LOG_INF("CLOUD_EVT_ERROR");
		break;
	case CLOUD_EVT_DATA_SENT:
		LOG_INF("CLOUD_EVT_DATA_SENT");
		break;
	case CLOUD_EVT_DATA_RECEIVED: {
		int err;

		LOG_INF("CLOUD_EVT_DATA_RECEIVED");
	}
	case CLOUD_EVT_PAIR_REQUEST:
		LOG_INF("CLOUD_EVT_PAIR_REQUEST");

                //Remove BLE devices if device was deleted.
                ble_conn_mgr_clear_desired();
                ble_conn_mgr_update_connections();
                ble_clear_discover_inprogress();

		on_user_pairing_req(evt);
		break;
	case CLOUD_EVT_PAIR_DONE:
		LOG_INF("CLOUD_EVT_PAIR_DONE");
		on_pairing_done();
		break;
	case CLOUD_EVT_FOTA_DONE:
		LOG_INF("CLOUD_EVT_FOTA_DONE");
#if defined(CONFIG_LTE_LINK_CONTROL)
		lte_lc_power_off();
#endif
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		LOG_WRN("Unknown cloud event type: %d", evt->type);
		break;
	}
}

void connection_evt_handler(const struct cloud_event *const evt)
{
	if (evt->type == CLOUD_EVT_CONNECTING) {
		LOG_INF("CLOUD_EVT_CONNECTING");
		ui_led_set_pattern(UI_CLOUD_CONNECTING);
		k_delayed_work_cancel(&cloud_reboot_work);

		if (evt->data.err != CLOUD_CONNECT_RES_SUCCESS) {

			cloud_connect_error_handler(evt->data.err);
		}
		return;
	} else if (evt->type == CLOUD_EVT_CONNECTED) {
		LOG_INF("CLOUD_EVT_CONNECTED");
		k_delayed_work_cancel(&cloud_reboot_work);
		atomic_set(&cloud_connect_attempts, 0);
		ui_led_set_pattern(UI_CLOUD_CONNECTED);
#if defined(CONFIG_CLOUD_PERSISTENT_SESSIONS)
		LOG_INF("Persistent Sessions = %u",
			evt->data.persistent_session);
#endif
	} else if (evt->type == CLOUD_EVT_DISCONNECTED) {
		s32_t connect_wait_s = CONFIG_CLOUD_CONNECT_RETRY_DELAY;

		LOG_INF("CLOUD_EVT_DISCONNECTED: %d", evt->data.err);
		ui_led_set_pattern(UI_LTE_CONNECTED);

		switch (evt->data.err)
		{
		case CLOUD_DISCONNECT_ERR_CLOSED_BY_REMOTE:
			if ((atomic_get(&cloud_connect_attempts) == 1) &&
			    (atomic_get(&cloud_association) ==
			     CLOUD_ASSOCIATION_STATE_INIT)) {
				LOG_INF("Disconnected by the cloud.");
				LOG_INF("This can occur during provisioning or when the device has invalid certificates.");
				connect_wait_s = 10;
			}
			break;
		case CLOUD_DISCONNECT_ERR_USER_REQUEST:
			if (atomic_get(&cloud_association) ==
			    CLOUD_ASSOCIATION_STATE_RECONNECT ||
			    atomic_get(&cloud_association) ==
			    CLOUD_ASSOCIATION_STATE_REQUESTED) {
				connect_wait_s = 10;
			}
			break;
		case CLOUD_DISCONNECT_ERR_MISC:
		case CLOUD_DISCONNECT_ERR_INVALID_REQUEST:
		default:
			break;
		}
		connect_to_cloud(connect_wait_s);
	}
}

/**@brief Initializes and submits delayed work. */
static void work_init(void)
{

	k_delayed_work_init(&cloud_reboot_work, cloud_reboot_handler);
	k_delayed_work_init(&cycle_cloud_connection_work,
			    cycle_cloud_connection);
	k_delayed_work_init(&cloud_connect_work, cloud_connect_work_fn);

}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static int modem_configure(void)
{
#if defined(CONFIG_BSD_LIBRARY)
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
		int err;

		LOG_INF("Connecting to LTE network. ");
		LOG_INF("This may take several minutes.");
		ui_led_set_pattern(UI_LTE_CONNECTING);

		err = lte_lc_init_and_connect();
		if (err) {
			LOG_ERR("LTE link could not be established.");
			ui_led_set_pattern(UI_LED_ERROR_LTE_LC);
			return err;
		}

		LOG_INF("Connected to LTE network");
		ui_led_set_pattern(UI_LTE_CONNECTED);
	}
#endif
	return 0;
}

void handle_bsdlib_init_ret(void)
{
	#if defined(CONFIG_BSD_LIBRARY)
	int ret = bsdlib_get_init_ret();

	/* Handle return values relating to modem firmware update */
	switch (ret) {
	case MODEM_DFU_RESULT_OK:
		LOG_INF("MODEM UPDATE OK. Will run new firmware");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		LOG_ERR("MODEM UPDATE ERROR %d. Will run old firmware", ret);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		LOG_ERR("MODEM UPDATE FATAL ERROR %d. Modem failiure", ret);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		break;
	}
	#endif /* CONFIG_BSD_LIBRARY */
}

void main(void)
{
	int ret;

	LOG_INF("Gateway started; rebuilt: %s %s", __DATE__, __TIME__);
        ble_init();

	k_work_q_start(&application_work_q, application_stack_area,
		       K_THREAD_STACK_SIZEOF(application_stack_area),
		       CONFIG_APPLICATION_WORKQUEUE_PRIORITY);
	if (IS_ENABLED(CONFIG_WATCHDOG)) {
		watchdog_init_and_start(&application_work_q);
	}

	handle_bsdlib_init_ret();

	cloud_backend = cloud_get_binding("NRF_CLOUD");
	__ASSERT(cloud_backend != NULL, "nRF Cloud backend not found");

	ret = cloud_init(cloud_backend, cloud_event_handler);
	if (ret) {
		LOG_ERR("Cloud backend could not be initialized, error: %d",
			ret);
		cloud_error_handler(ret);
	}

#if defined(CONFIG_USE_UI_MODULE)
	ui_init(NULL);
#endif

	work_init();

	while (modem_configure() != 0) {
		LOG_WRN("Failed to establish LTE connection.");
		LOG_WRN("Will retry in %d seconds.",
				CONFIG_CLOUD_CONNECT_RETRY_DELAY);
		k_sleep(K_SECONDS(CONFIG_CLOUD_CONNECT_RETRY_DELAY));
	}

	connect_to_cloud(K_NO_WAIT);
}
