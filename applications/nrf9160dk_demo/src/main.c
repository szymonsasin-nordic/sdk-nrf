/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <kernel_structs.h>
#include <stdio.h>
#include <string.h>
#include <console/console.h>
#include <power/reboot.h>
#include <logging/log_ctrl.h>
#if defined(CONFIG_BSD_LIBRARY)
#include <modem/bsdlib.h>
#include <bsd.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#endif /* CONFIG_BSD_LIBRARY */
#include <net/cloud.h>
#include <net/socket.h>
#include <net/nrf_cloud.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <dfu/mcuboot.h>
#endif

#include "cloud_codec.h"
#include "ui.h"
#include "service_info.h"
#include <modem/at_cmd.h>
#include "watchdog.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(nrf9160dk_demo, CONFIG_NRF9160DK_DEMO_LOG_LEVEL);

#define CLOUD_CONNACK_WAIT_DURATION	K_SECONDS(CONFIG_CLOUD_WAIT_DURATION)

#if defined(CONFIG_BSD_LIBRARY) && \
!defined(CONFIG_LTE_LINK_CONTROL)
#error "Missing CONFIG_LTE_LINK_CONTROL"
#endif

#if defined(CONFIG_BSD_LIBRARY) && \
defined(CONFIG_LTE_AUTO_INIT_AND_CONNECT) && \
defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES)
#error "PROVISION_CERTIFICATES "
	"requires CONFIG_LTE_AUTO_INIT_AND_CONNECT to be disabled!"
#endif

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

#if CONFIG_MODEM_INFO
static struct rsrp_data rsrp = {
	.value = 0,
	.offset = MODEM_INFO_RSRP_OFFSET_VAL,
};
#endif /* CONFIG_MODEM_INFO */

/* Stack definition for application workqueue */
K_THREAD_STACK_DEFINE(application_stack_area,
		      CONFIG_APPLICATION_WORKQUEUE_STACK_SIZE);
static struct k_work_q application_work_q;
static struct cloud_backend *cloud_backend;

/* Sensor data */
static struct cloud_channel_data button_cloud_data;
static struct cloud_channel_data device_cloud_data = {
	.type = CLOUD_CHANNEL_DEVICE_INFO,
	.tag = 0x1
};

static struct cloud_channel_data msg_cloud_data = {
	.type = CLOUD_CHANNEL_MSG,
	.tag = 0x1
};

#if CONFIG_MODEM_INFO
static struct modem_param_info modem_param;
static struct cloud_channel_data signal_strength_cloud_data;
#endif /* CONFIG_MODEM_INFO */
static atomic_val_t send_data_enable;

/* Variable to keep track of nRF cloud user association request. */
static atomic_val_t association_requested;
static atomic_val_t reconnect_to_cloud;

/* Structures for work */
static struct k_work send_button_data_work;
static struct k_work send_msg_data_work;
static struct k_work send_modem_at_cmd_work;
static struct k_delayed_work cloud_reboot_work;
static struct k_delayed_work cycle_cloud_connection_work;
static struct k_work device_status_work;

#if defined(CONFIG_AT_CMD)
#define MODEM_AT_CMD_BUFFER_LEN (CONFIG_AT_CMD_RESPONSE_MAX_LEN + 1)
#else
#define MODEM_AT_CMD_NOT_ENABLED_STR "Error: AT Command driver is not enabled"
#define MODEM_AT_CMD_BUFFER_LEN (sizeof(MODEM_AT_CMD_NOT_ENABLED_STR))
#endif
#define MODEM_AT_CMD_RESP_TOO_BIG_STR "Error: AT Command response is too large to be sent"
#define MODEM_AT_CMD_MAX_RESPONSE_LEN (2000)
static char modem_at_cmd_buff[MODEM_AT_CMD_BUFFER_LEN];
static K_SEM_DEFINE(modem_at_cmd_sem, 1, 1);

#if CONFIG_MODEM_INFO
static struct k_delayed_work rsrp_work;
#endif /* CONFIG_MODEM_INFO */

enum error_type {
	ERROR_CLOUD,
	ERROR_BSD_RECOVERABLE,
	ERROR_LTE_LC,
	ERROR_SYSTEM_FAULT
};

/* Forward declaration of functions */
static void sensors_init(void);
static void work_init(void);
static void sensor_data_send(struct cloud_channel_data *data);
static void device_status_send(struct k_work *work);
static void cycle_cloud_connection(struct k_work *work);
static void send_signon_message(void);

static void shutdown_modem(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
	/* Turn off and shutdown modem */
	LOG_ERR("LTE link disconnect");
	int err = lte_lc_power_off();

	if (err) {
		LOG_ERR("lte_lc_power_off failed: %d", err);
	}
#endif /* CONFIG_LTE_LINK_CONTROL */
#if defined(CONFIG_BSD_LIBRARY)
	LOG_ERR("Shutdown modem");
	bsdlib_shutdown();
#endif
}

/**@brief nRF Cloud error handler. */
void error_handler(enum error_type err_type, int err_code)
{
	if (err_type == ERROR_CLOUD) {
		shutdown_modem();
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

void k_sys_fatal_error_handler(unsigned int reason,
			       const z_arch_esf_t *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	LOG_ERR("Running main.c error handler");
	error_handler(ERROR_SYSTEM_FAULT, reason);
	CODE_UNREACHABLE;
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

static void send_button_data_work_fn(struct k_work *work)
{
	sensor_data_send(&button_cloud_data);
}

static void send_msg_data_work_fn(struct k_work *work)
{
	sensor_data_send(&msg_cloud_data);
}

static void send_modem_at_cmd_work_fn(struct k_work *work)
{
	enum at_cmd_state state;
	int err;
	struct cloud_channel_data modem_data = {
		.type = CLOUD_CHANNEL_MODEM
	};
	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_MSG
	};
	size_t len = strlen(modem_at_cmd_buff);

	if (len == 0) {
		err = -ENOBUFS;
		state = AT_CMD_ERROR;
	} else {
#if defined(CONFIG_AT_CMD)
		err = at_cmd_write(modem_at_cmd_buff, modem_at_cmd_buff,
				sizeof(modem_at_cmd_buff), &state);
#else
		/* Proper error msg has already been copied to buffer */
		err = 0;
#endif
	}

	/* Get response length; same buffer was used for the response. */
	len = strlen(modem_at_cmd_buff);

	if (err) {
		len = snprintf(modem_at_cmd_buff, sizeof(modem_at_cmd_buff),
				"AT CMD error: %d, state %d", err, state);
	} else if (len == 0) {
		len = snprintf(modem_at_cmd_buff, sizeof(modem_at_cmd_buff),
				"OK\r\n");
	} else if (len > MODEM_AT_CMD_MAX_RESPONSE_LEN) {
		len = snprintf(modem_at_cmd_buff, sizeof(modem_at_cmd_buff),
				MODEM_AT_CMD_RESP_TOO_BIG_STR);
	}

	modem_data.data.buf = modem_at_cmd_buff;
	modem_data.data.len = len;

	err = cloud_encode_data(&modem_data, CLOUD_CMD_GROUP_COMMAND, &msg);
	if (err) {
		LOG_ERR("[%s:%d] cloud_encode_data failed with error %d",
			__func__, __LINE__, err);
	} else {
		err = cloud_send(cloud_backend, &msg);
		cloud_release_data(&msg);
		if (err) {
			LOG_ERR("[%s:%d] cloud_send failed with error %d",
				__func__, __LINE__, err);
		}
	}

	k_sem_give(&modem_at_cmd_sem);
}

/**@brief Send button presses to cloud */
static void button_send(u8_t button_num, bool pressed)
{
	static char data[3];

	if (!atomic_get(&send_data_enable)) {
		return;
	}

	data[0] = button_num + '0';
	if (pressed) {
		data[1] = '1';
	} else {
		data[1] = '0';
	}
	data[2] = '\0';

	button_cloud_data.data.buf = data;
	button_cloud_data.data.len = strlen(data);
	button_cloud_data.tag += 1;

	if (button_cloud_data.tag == 0) {
		button_cloud_data.tag = 0x1;
	}

	LOG_INF("Sending button event for button %d=%s", button_num,
		pressed ? "pressed" : "released");
	k_work_submit_to_queue(&application_work_q, &send_button_data_work);

	if ((button_num == 4) && pressed) {
		send_signon_message();
	}
}

/**@brief Send a text message to the cloud; work function
 * will need to free the duplicated string; duplication is done
 * in case message is in a temporary buffer
 */
static void msg_send(const char *message)
{
	if (!atomic_get(&send_data_enable)) {
		return;
	}

	msg_cloud_data.data.buf = strdup(message);
	msg_cloud_data.data.len = strlen(message);
	msg_cloud_data.tag += 1;

	if (msg_cloud_data.tag == 0) {
		msg_cloud_data.tag = 0x1;
	}

	LOG_INF("Sending message: %s", message);
	k_work_submit_to_queue(&application_work_q, &send_msg_data_work);
}

static void cloud_cmd_handle_modem_at_cmd(const char * const at_cmd)
{
	if (!at_cmd) {
		return;
	}

	if (k_sem_take(&modem_at_cmd_sem, K_MSEC(20)) != 0) {
		LOG_ERR("[%s:%d] Modem AT cmd in progress.", __func__,
		       __LINE__);
		return;
	}

#if defined(CONFIG_AT_CMD)
	const size_t max_cmd_len = sizeof(modem_at_cmd_buff);

	if (strnlen(at_cmd, max_cmd_len) == max_cmd_len) {
		LOG_ERR("[%s:%d] AT cmd is too long, max length is %zu",
		       __func__, __LINE__, max_cmd_len - 1);
		/* Empty string will be handled as an error */
		modem_at_cmd_buff[0] = '\0';
	} else {
		strcpy(modem_at_cmd_buff, at_cmd);
	}
#else
	snprintf(modem_at_cmd_buff, sizeof(modem_at_cmd_buff),
			MODEM_AT_CMD_NOT_ENABLED_STR);
#endif

	k_work_submit_to_queue(&application_work_q, &send_modem_at_cmd_work);
}

static void cloud_cmd_handler(struct cloud_command *cmd)
{
	if ((cmd->channel == CLOUD_CHANNEL_MODEM) &&
		   (cmd->group == CLOUD_CMD_GROUP_COMMAND) &&
		   (cmd->type == CLOUD_CMD_DATA_STRING)) {
		LOG_INF("Received AT cmd from cloud: %s",
			log_strdup(cmd->data.data_string));
		cloud_cmd_handle_modem_at_cmd(cmd->data.data_string);
	} else if ((cmd->channel == CLOUD_CHANNEL_LED) &&
		   (cmd->group == CLOUD_CMD_GROUP_CFG_SET) &&
		   (cmd->type == CLOUD_CMD_STATE)) {
		u8_t led1 = (((u32_t)cmd->data.sv.value) & 0x00FF) != 0;
		u8_t led2 = (((u32_t)cmd->data.sv.value) & 0xFF00) != 0;

		LOG_INF("Received LED STATE cmd from cloud: [%u, %u]",
			led1, led2);
		ui_led_set_state(UI_LED_1, led1);
		ui_led_set_state(UI_LED_2, led2);
	} else if ((cmd->channel == CLOUD_CHANNEL_DEVICE_INFO) &&
		   (cmd->group == CLOUD_CMD_GROUP_GET) &&
		   (cmd->type == CLOUD_CMD_EMPTY)) {
		k_work_submit_to_queue(&application_work_q,
				       &device_status_work);
	} else if ((cmd->channel == CLOUD_CHANNEL_LTE_LINK_RSRP) &&
		   (cmd->group == CLOUD_CMD_GROUP_GET) &&
		   (cmd->type == CLOUD_CMD_EMPTY)) {
#if CONFIG_MODEM_INFO
		k_delayed_work_submit_to_queue(&application_work_q, &rsrp_work,
					       K_NO_WAIT);
#endif
	} else if ((cmd->group == CLOUD_CMD_GROUP_CFG_SET) &&
			   (cmd->type == CLOUD_CMD_INTERVAL)) {
		LOG_ERR("Interval command not valid for channel %d",
			cmd->channel);
	} else {
		LOG_ERR("bad command: channel %d:%s, group %d:%s, type %d:%s",
			cmd->channel, cloud_get_channel_name(cmd->channel),
			cmd->group, cloud_get_group_name(cmd->group),
			cmd->type, cloud_get_type_name(cmd->type));
	}
}

#if CONFIG_MODEM_INFO
/**@brief Callback handler for LTE RSRP data. */
static void modem_rsrp_handler(char rsrp_value)
{
	rsrp.value = rsrp_value;

	/* If the RSRP value is 255, it's documented as 'not known or not
	 * detectable'. Therefore, we should not send those values.
	 */
	if (rsrp.value == 255) {
		return;
	}

	/* Only send the RSRP if transmission is not already scheduled.
	 * Checking CONFIG_HOLD_TIME_RSRP gives the compiler a shortcut.
	 */
	if (CONFIG_HOLD_TIME_RSRP == 0 ||
	    k_delayed_work_remaining_get(&rsrp_work) == 0) {
		k_delayed_work_submit_to_queue(&application_work_q, &rsrp_work,
					       K_NO_WAIT);
	}
}

/**@brief Publish RSRP data to the cloud. */
static void modem_rsrp_data_send(struct k_work *work)
{
	char buf[CONFIG_MODEM_INFO_BUFFER_SIZE] = {0};
	static s32_t rsrp_prev; /* RSRP value last sent to cloud */
	s32_t rsrp_current;
	size_t len;

	if (!atomic_get(&send_data_enable)) {
		return;
	}

	/* The RSRP value is copied locally to avoid any race */
	rsrp_current = rsrp.value - rsrp.offset;

	if (rsrp_current == rsrp_prev) {
		return;
	}

	len = snprintf(buf, CONFIG_MODEM_INFO_BUFFER_SIZE,
		       "%d", rsrp_current);

	signal_strength_cloud_data.data.buf = buf;
	signal_strength_cloud_data.data.len = len;
	signal_strength_cloud_data.tag += 1;

	if (signal_strength_cloud_data.tag == 0) {
		signal_strength_cloud_data.tag = 0x1;
	}

	sensor_data_send(&signal_strength_cloud_data);
	rsrp_prev = rsrp_current;

	if (CONFIG_HOLD_TIME_RSRP > 0) {
		k_delayed_work_submit_to_queue(&application_work_q, &rsrp_work,
					      K_SECONDS(CONFIG_HOLD_TIME_RSRP));
	}
}
#endif /* CONFIG_MODEM_INFO */

/**@brief Poll device info and send data to the cloud. */
static void device_status_send(struct k_work *work)
{
	if (!atomic_get(&send_data_enable)) {
		return;
	}

	cJSON *root_obj = cJSON_CreateObject();

	if (root_obj == NULL) {
		LOG_ERR("Unable to allocate JSON object");
		return;
	}

	size_t item_cnt = 0;

#ifdef CONFIG_MODEM_INFO
	int ret = modem_info_params_get(&modem_param);

	if (ret < 0) {
		LOG_ERR("Unable to obtain modem parameters: %d", ret);
	} else {
		ret = modem_info_json_object_encode(&modem_param, root_obj);
		if (ret > 0) {
			item_cnt = (size_t)ret;
		}
	}
#endif /* CONFIG_MODEM_INFO */

	const char *const ui[] = {
		CLOUD_CHANNEL_STR_BUTTON,
		CLOUD_CHANNEL_STR_MSG,
#if IS_ENABLED(CONFIG_MODEM_INFO)
		CLOUD_CHANNEL_STR_LTE_LINK_RSRP,
#endif
	};

	const char *const fota[] = {
#if defined(CONFIG_CLOUD_FOTA_APP)
		SERVICE_INFO_FOTA_STR_APP,
#endif
#if defined(CONFIG_CLOUD_FOTA_MODEM)
		SERVICE_INFO_FOTA_STR_MODEM
#endif
	};

	if (service_info_json_object_encode(ui, ARRAY_SIZE(ui),
					    fota, ARRAY_SIZE(fota),
					    SERVICE_INFO_FOTA_VER_CURRENT,
					    root_obj) == 0) {
		++item_cnt;
	}

	if (item_cnt == 0) {
		cJSON_Delete(root_obj);
		return;
	}

	device_cloud_data.data.buf = (char *)root_obj;
	device_cloud_data.data.len = item_cnt;
	device_cloud_data.tag += 1;

	if (device_cloud_data.tag == 0) {
		device_cloud_data.tag = 0x1;
	}

	/* Transmits the data to the cloud. Frees the JSON object. */
	sensor_data_send(&device_cloud_data);
}

/**@brief Send sensor data to nRF Cloud. **/
static void sensor_data_send(struct cloud_channel_data *data)
{
	int err = 0;
	struct cloud_msg msg = {
			.qos = CLOUD_QOS_AT_MOST_ONCE,
			.endpoint.type = CLOUD_EP_TOPIC_MSG
		};

	if (data->type == CLOUD_CHANNEL_DEVICE_INFO) {
		msg.endpoint.type = CLOUD_EP_TOPIC_STATE;
	}

	if (!atomic_get(&send_data_enable)) {
		return;
	}

	if (data->type == CLOUD_CHANNEL_DEVICE_INFO) {
		err = cloud_encode_digital_twin_data(data, &msg);
	} else {
		err = cloud_encode_data(data, CLOUD_CMD_GROUP_DATA, &msg);
	}

	if (err) {
		LOG_ERR("Unable to encode cloud data: %d", err);
	}

	err = cloud_send(cloud_backend, &msg);

	if (data->type == CLOUD_CHANNEL_MSG) {
		if (data->data.len) {
			free(data->data.buf);
			data->data.buf = NULL;
			data->data.len = 0;
		}
	}

	cloud_release_data(&msg);

	if (err) {
		LOG_ERR("%s failed: %d", __func__, err);
		cloud_error_handler(err);
	}
}

/**@brief Reboot the device if CONNACK has not arrived. */
static void cloud_reboot_handler(struct k_work *work)
{
	error_handler(ERROR_CLOUD, -ETIMEDOUT);
}

/**@brief Callback for sensor attached event from nRF Cloud. */
void sensors_start(void)
{
	atomic_set(&send_data_enable, 1);
	sensors_init();
}

/**@brief nRF Cloud specific callback for cloud association event. */
static void on_user_pairing_req(const struct cloud_event *evt)
{
	if (atomic_get(&association_requested) == 0) {
		atomic_set(&association_requested, 1);
		ui_led_set_pattern(UI_CLOUD_PAIRING);
		LOG_INF("Add device to cloud account.");
		LOG_INF("Waiting for cloud association...");

		/* If the association is not done soon enough (< ~5 min?) */
		/* a connection cycle is needed... TBD why. */
		k_delayed_work_submit_to_queue(&application_work_q,
					&cycle_cloud_connection_work,
					CONN_CYCLE_AFTER_ASSOCIATION_REQ_MS);
	}
}

static void cycle_cloud_connection(struct k_work *work)
{
	int err;
	s32_t reboot_wait_ms = REBOOT_AFTER_DISCONNECT_WAIT_MS;

	LOG_INF("Disconnecting from cloud...");

	err = cloud_disconnect(cloud_backend);
	if (err == 0) {
		atomic_set(&reconnect_to_cloud, 1);
	} else {
		reboot_wait_ms = K_SECONDS(5);
		LOG_INF("Disconnect failed. Device will reboot in %d seconds",
			(reboot_wait_ms/MSEC_PER_SEC));
	}

	/* Reboot fail-safe on disconnect */
	k_delayed_work_submit_to_queue(&application_work_q, &cloud_reboot_work,
				       reboot_wait_ms);
}

/** @brief Handle procedures after successful association with nRF Cloud. */
void on_pairing_done(void)
{
	if (atomic_get(&association_requested)) {
		atomic_set(&association_requested, 0);
		k_delayed_work_cancel(&cycle_cloud_connection_work);

		/* After successful association, the device must */
		/* reconnect to the cloud. */
		LOG_INF("Device associated with cloud.");
		LOG_INF("Reconnecting for cloud policy to take effect.");
		cycle_cloud_connection(NULL);
	}
}

/** @brief Send helpful text to 9160DK user in nRFCloud
 *         Terminal card
 */
static void send_signon_message(void)
{
	msg_send(
	     "**Welcome to the Nordic Semiconductor nrf9160 Development Kit**\n"
	     "  1. Change buttons or switches to receive messages below.\n"
	     "  2. Type commands in the Send a message box:\n"
	     "       Use the state array [LED1,LED2] to turn on(1) or off(0).\n"
	     "       For example, to set LED1 on and LED2 off:\n"
	     "          {\"appId\":\"LED\",\n"
	     "           \"messageType\":\"CFG_SET\",\n"
	     "           \"data\": {\"state\":[1, 0]} }\n"
	     "  3. Use the FOTA update service to try out other examples.\n"
	     "  Getting started can be found here: https://bit.ly/37NMvuo");
}

void cloud_event_handler(const struct cloud_backend *const backend,
			 const struct cloud_event *const evt,
			 void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case CLOUD_EVT_CONNECTED:
		LOG_INF("CLOUD_EVT_CONNECTED");
		k_delayed_work_cancel(&cloud_reboot_work);
		ui_led_set_pattern(UI_CLOUD_CONNECTED);
		break;
	case CLOUD_EVT_READY:
		LOG_INF("CLOUD_EVT_READY");
		ui_led_set_pattern(UI_CLOUD_CONNECTED);

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		/* Mark image as good to avoid rolling back after update */
		boot_write_img_confirmed();
#endif

		sensors_start();
		send_signon_message();
		break;
	case CLOUD_EVT_DISCONNECTED:
		LOG_INF("CLOUD_EVT_DISCONNECTED");
		ui_led_set_pattern(UI_LTE_DISCONNECTED);
		/* Expect an error event (POLLNVAL) on the cloud socket poll */
		/* Handle reconnect there if desired */
		break;
	case CLOUD_EVT_ERROR:
		LOG_INF("CLOUD_EVT_ERROR");
		break;
	case CLOUD_EVT_DATA_SENT:
		LOG_INF("CLOUD_EVT_DATA_SENT");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		LOG_INF("CLOUD_EVT_DATA_RECEIVED: %s",
			log_strdup(evt->data.msg.buf));
		int err = cloud_decode_command(evt->data.msg.buf);

		if (err == -ENOTSUP) {
			LOG_ERR("Unsupported command");
		}
		break;
	case CLOUD_EVT_PAIR_REQUEST:
		LOG_INF("CLOUD_EVT_PAIR_REQUEST");
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

/**@brief Initializes and submits delayed work. */
static void work_init(void)
{
	k_work_init(&send_button_data_work, send_button_data_work_fn);
	k_work_init(&send_msg_data_work, send_msg_data_work_fn);
	k_work_init(&send_modem_at_cmd_work, send_modem_at_cmd_work_fn);
	k_delayed_work_init(&cloud_reboot_work, cloud_reboot_handler);
	k_delayed_work_init(&cycle_cloud_connection_work,
			    cycle_cloud_connection);
	k_work_init(&device_status_work, device_status_send);
#if CONFIG_MODEM_INFO
	k_delayed_work_init(&rsrp_work, modem_rsrp_data_send);
#endif /* CONFIG_MODEM_INFO */
}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
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
			error_handler(ERROR_LTE_LC, err);
		}

		LOG_INF("Connected to LTE network");
		ui_led_set_pattern(UI_LTE_CONNECTED);
	}
#endif
}

static void button_sensor_init(void)
{
	button_cloud_data.type = CLOUD_CHANNEL_BUTTON;
	button_cloud_data.tag = 0x1;
}

#if CONFIG_MODEM_INFO
/**brief Initialize LTE status containers. */
static void modem_data_init(void)
{
	int err;

	err = modem_info_init();
	if (err) {
		LOG_ERR("Modem info could not be established: %d", err);
		return;
	}

	modem_info_params_init(&modem_param);

	signal_strength_cloud_data.type = CLOUD_CHANNEL_LTE_LINK_RSRP;
	signal_strength_cloud_data.tag = 0x1;

	modem_info_rsrp_register(modem_rsrp_handler);
}
#endif /* CONFIG_MODEM_INFO */

/**@brief Initializes the sensors that are used by the application. */
static void sensors_init(void)
{
#if CONFIG_MODEM_INFO
	modem_data_init();
#endif /* CONFIG_MODEM_INFO */

	k_work_submit_to_queue(&application_work_q, &device_status_work);

	button_sensor_init();
}

/**@brief User interface event handler. */
static void ui_evt_handler(struct ui_evt evt)
{
	button_send(evt.button, evt.type == UI_EVT_BUTTON_ACTIVE ? 1 : 0);
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

	LOG_INF("nRF9160DK Demo Started");
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

	ui_init(ui_evt_handler);

	ret = cloud_decode_init(cloud_cmd_handler);
	if (ret) {
		LOG_ERR("Cloud command decoder initialization error: %d", ret);
		cloud_error_handler(ret);
	}

	work_init();
	modem_configure();
connect:
	ret = cloud_connect(cloud_backend);
	if (ret != CLOUD_CONNECT_RES_SUCCESS) {
		cloud_connect_error_handler(ret);
	} else {
		atomic_set(&reconnect_to_cloud, 0);
		k_delayed_work_submit_to_queue(&application_work_q,
					       &cloud_reboot_work,
					       CLOUD_CONNACK_WAIT_DURATION);
	}

	struct pollfd fds[] = {
		{
			.fd = cloud_backend->config->socket,
			.events = POLLIN
		}
	};

	while (true) {
		ret = poll(fds, ARRAY_SIZE(fds),
			   cloud_keepalive_time_left(cloud_backend));
		if (ret < 0) {
			LOG_ERR("poll() returned an error: %d", ret);
			error_handler(ERROR_CLOUD, ret);
			continue;
		}

		if (ret == 0) {
			cloud_ping(cloud_backend);
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			cloud_input(cloud_backend);
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			if (atomic_get(&reconnect_to_cloud)) {
				k_delayed_work_cancel(&cloud_reboot_work);
				LOG_INF("Attempting reconnect...");
				goto connect;
			}
			LOG_ERR("Socket error: POLLNVAL");
			LOG_ERR("The cloud socket was unexpectedly closed.");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_ERR("Socket error: POLLHUP");
			LOG_ERR("Connection was closed by the cloud.");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_ERR("Socket error: POLLERR");
			LOG_ERR("Cloud connection was unexpectedly closed.");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}
	}

	cloud_disconnect(cloud_backend);
	goto connect;
}
