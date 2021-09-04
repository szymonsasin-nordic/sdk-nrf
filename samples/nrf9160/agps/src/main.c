/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <stdio.h>
#include <modem/lte_lc.h>
#include <net/cloud.h>
#include <net/socket.h>
#include <dk_buttons_and_leds.h>
#include <drivers/gps.h>
#include <modem/agps.h>
#include <sys/reboot.h>
#include <modem/at_cmd.h>
#if defined(CONFIG_NRF_CLOUD_PGPS)
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include <date_time.h>
#include <pm_config.h>
#endif

#include <logging/log.h>

#define LOGSTRDUP //log_strdup
#define SERVICE_INFO_GPS "{\"state\":{\"reported\":{\"device\": \
			  {\"serviceInfo\":{\"ui\":[\"GPS\"]}}}}}"

LOG_MODULE_REGISTER(agps_sample, CONFIG_AGPS_SAMPLE_LOG_LEVEL);

static struct cloud_backend *cloud_backend;
static const struct device *gps_dev;
static uint64_t start_search_timestamp;
static uint64_t fix_timestamp;
static struct k_work gps_start_work;
static struct k_work_delayable reboot_work;
#if defined(CONFIG_NRF_CLOUD_PGPS)
static struct k_work manage_pgps_work;
static struct k_work notify_pgps_work;
static struct gps_agps_request agps_request;
#endif
static uint32_t pre_gps_tx;
static uint32_t pre_gps_rx;
static uint32_t post_gps_tx;
static uint32_t post_gps_rx;

static void gps_start_work_fn(struct k_work *work);

int enable_data_stats(bool start)
{
	enum at_cmd_state at_state;
	char buf[256];
	char cmd[120];

	snprintf(cmd, sizeof(cmd), "AT%%XCONNSTAT=%d", start ? 1 : 0);

	//printk("sending: %s\n", cmd);
	int err = at_cmd_write(cmd, buf, sizeof(buf),
				&at_state);

	//printk("received: %s\n", buf);
	if (err) {
		printk("Error when trying to do at_cmd_write: %d, at_state: %d",
			err, at_state);
		printk("Started collecting data transfer stats\n");
	}
	return err;
}

int get_data_stats(uint32_t *data_tx, uint32_t *data_rx)
{
	enum at_cmd_state at_state;
	char buf[256];
	char tmp[256];
	uint32_t sms_tx;
	uint32_t sms_rx;
	uint32_t pkt_max;
	uint32_t pkt_ave;

	//printk("sending: AT%%XCONNSTAT?\n");
	int err = at_cmd_write("AT%XCONNSTAT?", buf, sizeof(buf),
				&at_state);

	//printk("received: %s\n", buf);
	if (err) {
		printk("Error when trying to do at_cmd_write: %d, at_state: %d",
			err, at_state);
	} else {
		sscanf(buf, "%s%d,%d,%d,%d,%d,%d", tmp,
		       &sms_tx, &sms_rx, data_tx, data_rx, &pkt_max, &pkt_ave);
		*data_tx *= 1024; /* convert to bytes */
		*data_rx *= 1024;
	}
	return err;
}

#if defined(CONFIG_NRF_CLOUD_PGPS)
static struct nrf_cloud_pgps_prediction *prediction;

void pgps_handler(struct nrf_cloud_pgps_event *event)
{
	/* GPS unit asked for it, but we didn't have it; check now */
	printk("P-GPS event type: %d\n", event->type);

	if (event->type == PGPS_EVT_AVAILABLE) {
		prediction = event->prediction;

		k_work_submit(&manage_pgps_work);
	}
}

static void manage_pgps(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	printk("Sending prediction to modem...\n");
	err = nrf_cloud_pgps_inject(prediction, &agps_request, NULL);
	if (err) {
		printk("Unable to send prediction to modem: %d\n", err);
	}

	err = nrf_cloud_pgps_preemptive_updates();
	if (err) {
		printk("Error requesting updates: %d\n", err);
	}
}

static void notify_pgps(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	err = nrf_cloud_pgps_notify_prediction();
	if (err) {
		printk("error requesting notification of prediction availability: %d\n", err);
	}
}
#endif

static void cloud_send_msg(void)
{
	int err;

	//LOG_DBG("Publishing message: %s\n", LOGSTRDUP(CONFIG_CLOUD_MESSAGE));

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_MSG,
		.buf = CONFIG_CLOUD_MESSAGE,
		.len = sizeof(CONFIG_CLOUD_MESSAGE)
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		printk("cloud_send failed, error: %d\n", err);
	}
}

static void gps_start_work_fn(struct k_work *work)
{
	int err;
	struct gps_config gps_cfg = {
		.nav_mode = GPS_NAV_MODE_PERIODIC,
		.power_mode = GPS_POWER_MODE_DISABLED,
		.timeout = 120,
		.interval = 240,
		.priority = true,
	};

	ARG_UNUSED(work);

#if defined(CONFIG_NRF_CLOUD_PGPS)
	static bool initialized;

	if (!initialized) {
		struct nrf_cloud_pgps_init_param param = {
			.event_handler = pgps_handler,
			.storage_base = PM_MCUBOOT_SECONDARY_ADDRESS,
			.storage_size = PM_MCUBOOT_SECONDARY_SIZE};

		err = nrf_cloud_pgps_init(&param);
		if (err) {
			printk("Error from PGPS init: %d\n", err);
		} else {
			initialized = true;
		}
	}
#endif

	err = gps_start(gps_dev, &gps_cfg);
	if (err) {
		printk("Failed to start GPS, error: %d\n", err);
		return;
	}

	printk("Periodic GPS search started with interval %d s, timeout %d s\n",
		gps_cfg.interval, gps_cfg.timeout);
}

#if defined(CONFIG_AGPS)
/* Converts the A-GPS data request from GPS driver to GNSS API format. */
static void agps_request_convert(
	struct nrf_modem_gnss_agps_data_frame *dest,
	const struct gps_agps_request *src)
{
	dest->sv_mask_ephe = src->sv_mask_ephe;
	dest->sv_mask_alm = src->sv_mask_alm;
	dest->data_flags = 0;
	if (src->utc) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST;
	}
	if (src->klobuchar) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST;
	}
	if (src->nequick) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST;
	}
	if (src->system_time_tow) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST;
	}
	if (src->position) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_POSITION_REQUEST;
	}
	if (src->integrity) {
		dest->data_flags |= NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST;
	}
}
#endif

static void on_agps_needed(struct gps_agps_request request)
{
#if defined(CONFIG_AGPS)
	struct nrf_modem_gnss_agps_data_frame agps_request;

	agps_request_convert(&agps_request, &request);

	int err = agps_request_send(agps_request, AGPS_SOCKET_NOT_PROVIDED);

	if (err) {
		printk("Failed to request A-GPS data, error: %d\n", err);
		return;
	}
#endif
#if defined(CONFIG_NRF_CLOUD_PGPS)
	/* AGPS data not expected, so move on to PGPS */
	if (!nrf_cloud_agps_request_in_progress()) {
		k_work_submit(&notify_pgps_work);
	}
#endif
}

static void send_service_info(void)
{
	int err;
	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_STATE,
		.buf = SERVICE_INFO_GPS,
		.len = strlen(SERVICE_INFO_GPS)
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		printk("Failed to send message to cloud, error: %d\n", err);
		return;
	}

	printk("Service info sent to cloud\n");
}

static void cloud_event_handler(const struct cloud_backend *const backend,
				const struct cloud_event *const evt,
				void *user_data)
{
	int err = 0;

	ARG_UNUSED(backend);
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case CLOUD_EVT_CONNECTING:
		printk("CLOUD_EVT_CONNECTING\n");
		break;
	case CLOUD_EVT_CONNECTED:
		printk("CLOUD_EVT_CONNECTED\n");
		break;
	case CLOUD_EVT_READY:
		printk("CLOUD_EVT_READY\n");
		/* Update nRF Cloud with GPS service info signifying that it
		 * has GPS capabilities. */
		send_service_info();
		if (get_data_stats(&pre_gps_tx, &pre_gps_rx) == 0) {
			printk("PRE GPS data_tx:%u, data_rx:%u\n",
			       pre_gps_tx, pre_gps_rx);
		}
		k_work_submit(&gps_start_work);
		break;
	case CLOUD_EVT_DISCONNECTED:
		printk("CLOUD_EVT_DISCONNECTED\n");
		break;
	case CLOUD_EVT_ERROR:
		printk("CLOUD_EVT_ERROR\n");
		break;
	case CLOUD_EVT_DATA_SENT:
		printk("CLOUD_EVT_DATA_SENT\n");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		printk("CLOUD_EVT_DATA_RECEIVED\n");

		/* Convenience functionality for remote testing.
		 * The device is reset if it receives "{"reboot":true}"
		 * from the cloud. The command can be sent using the terminal
		 * card on the device page on nrfcloud.com.
		 */
		if (evt->data.msg.buf[0] == '{') {
			int ret = strncmp(evt->data.msg.buf,
				      "{\"reboot\":true}",
				      strlen("{\"reboot\":true}"));

			if (ret == 0) {
				/* The work item may already be scheduled
				 * because of the button being pressed,
				 * so use rescheduling here to get the work
				 * submitted with no delay.
				 */
				k_work_reschedule(&reboot_work, K_NO_WAIT);
			}
			break;
		}

#if defined(CONFIG_AGPS)
		err = agps_cloud_data_process(evt->data.msg.buf, evt->data.msg.len);
		if (!err) {
			printk("A-GPS data processed\n");
#if defined(CONFIG_NRF_CLOUD_PGPS)
			/* call us back when prediction is ready */
			k_work_submit(&notify_pgps_work);
#endif
			/* data was valid; no need to pass to other handlers */
			break;
		}
#endif
#if defined(CONFIG_NRF_CLOUD_PGPS)
		err = nrf_cloud_pgps_process(evt->data.msg.buf, evt->data.msg.len);
		if (err) {
			printk("Error processing PGPS packet: %d\n", err);
		}
#else
		if (err) {
			printk("Unable to process agps data, error: %d\n", err);
		}
#endif
		break;
	case CLOUD_EVT_PAIR_REQUEST:
		printk("CLOUD_EVT_PAIR_REQUEST\n");
		break;
	case CLOUD_EVT_PAIR_DONE:
		printk("CLOUD_EVT_PAIR_DONE\n");
		break;
	case CLOUD_EVT_FOTA_DONE:
		printk("CLOUD_EVT_FOTA_DONE\n");
		break;
	case CLOUD_EVT_FOTA_ERROR:
		printk("CLOUD_EVT_FOTA_ERROR\n");
		break;
	default:
		printk("Unknown cloud event type: %d\n", evt->type);
		break;
	}
}

static void print_pvt_data(struct gps_pvt *pvt_data)
{
	char buf[300];
	size_t len;

	len = snprintf(buf, sizeof(buf),
		      "\r\n\tLongitude:  %f\r\n\t"
		      "Latitude:   %f\r\n\t"
		      "Altitude:   %f\r\n\t"
		      "Speed:      %f\r\n\t"
		      "Heading:    %f\r\n\t"
		      "Date:       %02u-%02u-%02u\r\n\t"
		      "Time (UTC): %02u:%02u:%02u\r\n",
		      pvt_data->longitude, pvt_data->latitude,
		      pvt_data->altitude, pvt_data->speed, pvt_data->heading,
		      pvt_data->datetime.year, pvt_data->datetime.month,
		      pvt_data->datetime.day, pvt_data->datetime.hour,
		      pvt_data->datetime.minute, pvt_data->datetime.seconds);
	if (len < 0) {
		printk("Could not construct PVT print\n");
	} else {
		printk("%s\n", LOGSTRDUP(buf));
	}
}

static void print_satellite_stats(struct gps_pvt *pvt_data)
{
	uint8_t tracked = 0;
	uint32_t tracked_sats = 0;
	static uint32_t prev_tracked_sats;
	char print_buf[100];
	size_t print_buf_len;

	for (int i = 0; i < GPS_PVT_MAX_SV_COUNT; ++i) {
		if ((pvt_data->sv[i].sv > 0) &&
		    (pvt_data->sv[i].sv < 33)) {
			tracked++;
			tracked_sats |= BIT(pvt_data->sv[i].sv - 1);
		}
	}

	if ((tracked_sats == 0) || (tracked_sats == prev_tracked_sats)) {
		if (tracked_sats != prev_tracked_sats) {
			prev_tracked_sats = tracked_sats;
			//LOG_DBG("Tracking no satellites\n");
		}

		return;
	}

	prev_tracked_sats = tracked_sats;
	print_buf_len = snprintk(print_buf, sizeof(print_buf), "Tracking:  ");

	for (size_t i = 0; i < 32; i++) {
		if (tracked_sats & BIT(i)) {
			print_buf_len +=
				snprintk(&print_buf[print_buf_len - 1],
					 sizeof(print_buf) - print_buf_len,
					 "%d  ", i + 1);
			if (print_buf_len < 0) {
				printk("Failed to print satellite stats\n");
				break;
			}
		}
	}

	printk("%s\n", LOGSTRDUP(print_buf));
	//LOG_DBG("Searching for %lld seconds\n", (k_uptime_get() - start_search_timestamp) / 1000);
}

static void send_nmea(char *nmea)
{
	int err;
	char buf[150];
	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_MSG,
		.buf = buf,
	};

	msg.len = snprintf(buf, sizeof(buf),
		"{"
			"\"appId\":\"GPS\","
			"\"data\":\"%s\","
			"\"messageType\":\"DATA\""
		"}", nmea);
	if (msg.len < 0) {
		printk("Failed to create GPS cloud message\n");
		return;
	}

	if (get_data_stats(&post_gps_tx, &post_gps_rx) == 0) {
		printk("POST GPS data_tx:%u, data_rx:%u\n",
		       post_gps_tx, post_gps_rx);
		printk("DELTA    data_tx:%u, data_rx:%u\n",
		       post_gps_tx - pre_gps_tx,
		       post_gps_rx - pre_gps_rx);
		pre_gps_tx = post_gps_tx;
		pre_gps_rx = post_gps_rx;
		/* after this, measures sending to cloud usage */
	}

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		printk("Failed to send message to cloud, error: %d\n", err);
		return;
	}

	printk("GPS position sent to cloud\n");
}

static void gps_handler(const struct device *dev, struct gps_event *evt)
{
	ARG_UNUSED(dev);

	switch (evt->type) {
	case GPS_EVT_SEARCH_STARTED:
		printk("GPS_EVT_SEARCH_STARTED\n");
		start_search_timestamp = k_uptime_get();
		break;
	case GPS_EVT_SEARCH_STOPPED:
		printk("GPS_EVT_SEARCH_STOPPED\n");
		break;
	case GPS_EVT_SEARCH_TIMEOUT:
		printk("GPS_EVT_SEARCH_TIMEOUT\n");
		break;
	case GPS_EVT_OPERATION_BLOCKED:
		printk("GPS_EVT_OPERATION_BLOCKED\n");
		break;
	case GPS_EVT_OPERATION_UNBLOCKED:
		printk("GPS_EVT_OPERATION_UNBLOCKED\n");
		break;
	case GPS_EVT_AGPS_DATA_NEEDED:
		printk("GPS_EVT_AGPS_DATA_NEEDED\n");
#if defined(CONFIG_NRF_CLOUD_PGPS)
		printk("A-GPS request from modem; emask:0x%08X amask:0x%08X utc:%u "
			"klo:%u neq:%u tow:%u pos:%u int:%u\n",
			evt->agps_request.sv_mask_ephe, evt->agps_request.sv_mask_alm,
			evt->agps_request.utc, evt->agps_request.klobuchar,
			evt->agps_request.nequick, evt->agps_request.system_time_tow,
			evt->agps_request.position, evt->agps_request.integrity);
		memcpy(&agps_request, &evt->agps_request, sizeof(agps_request));
#endif
		on_agps_needed(evt->agps_request);
		break;
	case GPS_EVT_PVT:
		print_satellite_stats(&evt->pvt);
		break;
	case GPS_EVT_PVT_FIX:
		fix_timestamp = k_uptime_get();

		printk("---------       FIX       ---------\n");
		printk("Time to fix: %d seconds\n",
			(uint32_t)(fix_timestamp - start_search_timestamp) / 1000);
		print_pvt_data(&evt->pvt);
		printk("-----------------------------------\n");
#if defined(CONFIG_NRF_CLOUD_PGPS) && defined(CONFIG_PGPS_STORE_LOCATION)
		nrf_cloud_pgps_set_location(evt->pvt.latitude, evt->pvt.longitude);
#endif
		break;
	case GPS_EVT_NMEA_FIX:
		send_nmea(evt->nmea.buf);
		break;
	default:
		break;
	}
}

static void reboot_work_fn(struct k_work *work)
{
	printk("Rebooting in 2 seconds...\n");
	k_sleep(K_SECONDS(2));
	sys_reboot(0);
}

static void work_init(void)
{
	k_work_init(&gps_start_work, gps_start_work_fn);
	k_work_init_delayable(&reboot_work, reboot_work_fn);
#if defined(CONFIG_NRF_CLOUD_PGPS)
	k_work_init(&manage_pgps_work, manage_pgps);
	k_work_init(&notify_pgps_work, notify_pgps);
#endif
}

static int modem_configure(void)
{
	int err = 0;

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
		printk("Connecting to LTE network. This may take minutes.\n");

#if defined(CONFIG_LTE_POWER_SAVING_MODE)
		err = lte_lc_psm_req(true);
		if (err) {
			printk("PSM request failed, error: %d\n", err);
			return err;
		}

		printk("PSM mode requested\n");
#endif

		err = lte_lc_init_and_connect();
		if (err) {
			printk("LTE link could not be established, error: %d\n",
				err);
			return err;
		}

		printk("Connected to LTE network\n");
	}

	return err;
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states & DK_BTN1_MSK) {
		cloud_send_msg();
		k_work_schedule(&reboot_work, K_SECONDS(3));
	} else if (has_changed & ~button_states & DK_BTN1_MSK) {
		k_work_cancel_delayable(&reboot_work);
	}
}

#if defined(CONFIG_DATE_TIME)
static void date_time_event_handler(const struct date_time_evt *evt)
{
	switch (evt->type) {
	case DATE_TIME_OBTAINED_MODEM:
		printk("DATE_TIME_OBTAINED_MODEM\n");
		break;
	case DATE_TIME_OBTAINED_NTP:
		printk("DATE_TIME_OBTAINED_NTP\n");
		break;
	case DATE_TIME_OBTAINED_EXT:
		printk("DATE_TIME_OBTAINED_EXT\n");
		break;
	case DATE_TIME_NOT_OBTAINED:
		printk("DATE_TIME_NOT_OBTAINED\n");
		break;
	default:
		break;
	}
}
#endif

void main(void)
{
	int err;

	printk("A-GPS sample has started\n");

	cloud_backend = cloud_get_binding("NRF_CLOUD");
	__ASSERT(cloud_backend, "Could not get binding to cloud backend");

	err = cloud_init(cloud_backend, cloud_event_handler);
	if (err) {
		printk("Cloud backend could not be initialized, error: %d\n",
			err);
		return;
	}

	work_init();

	err = modem_configure();
	if (err) {
		printk("Modem configuration failed with error %d\n",
			err);
		return;
	}

	gps_dev = device_get_binding("NRF9160_GPS");
	if (gps_dev == NULL) {
		printk("Could not get binding to nRF9160 GPS\n");
		return;
	}

	enable_data_stats(true);

	err = gps_init(gps_dev, gps_handler);
	if (err) {
		printk("Could not initialize GPS, error: %d\n", err);
		return;
	}

	err = dk_buttons_init(button_handler);
	if (err) {
		printk("Buttons could not be initialized, error: %d\n", err);
		printk("Continuing without button funcitonality\n");
	}

#if defined(CONFIG_DATE_TIME)
	date_time_update_async(date_time_event_handler);
#endif

	err = cloud_connect(cloud_backend);
	if (err) {
		printk("Cloud connection failed, error: %d\n", err);
		return;
	}

	/* The cloud connection is polled in a thread in the backend.
	 * Events will be received to cloud_event_handler() when data is
	 * received from the cloud.
	 */
}
