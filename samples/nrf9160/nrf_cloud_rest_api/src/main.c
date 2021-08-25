/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdlib.h>
#include <stdio.h>
#include <net/socket.h>
#include <net/net_ip.h>
#include <modem/nrf_modem_lib.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <net/tls_credentials.h>
#include <modem/modem_key_mgmt.h>
#include <net/http_client.h>
#include <sys/base64.h>
#include <net/nrf_cloud_rest.h>
#include <modem/modem_jwt.h>
#include <power/reboot.h>
#include <logging/log.h>
#include <dk_buttons_and_leds.h>
#include <net/fota_download.h>
#include <net/nrf_cloud_agps.h>
#include <drivers/gps.h>
#include <modem/agps.h>
#include <nrf_modem_gnss.h>

#if defined(CONFIG_REST_ID_SRC_COMPILE_TIME)
BUILD_ASSERT(sizeof(CONFIG_REST_DEVICE_ID) > 1, "Device ID must be specified");
#elif defined(CONFIG_REST_ID_SRC_IMEI)
#define CGSN_RSP_LEN 19
#define IMEI_LEN 15
#define IMEI_CLIENT_ID_LEN (sizeof(CONFIG_REST_ID_PREFIX) \
			    - 1 + IMEI_LEN)
BUILD_ASSERT(IMEI_CLIENT_ID_LEN <= NRF_CLOUD_CLIENT_ID_MAX_LEN,
	"REST_ID_PREFIX plus IMEI must not exceed NRF_CLOUD_CLIENT_ID_MAX_LEN");
#endif

LOG_MODULE_REGISTER(rest_sample, CONFIG_NRF_CLOUD_REST_SAMPLE_LOG_LEVEL);

#define FOTA_DL_FRAGMENT_SZ 1400
#define REST_RX_BUF_SZ	2100
#define BUTTON_EVENT_BTN_NUM CONFIG_REST_BUTTON_EVT_NUM
#define LED_NUM CONFIG_REST_LED_NUM
#define JITP_REQ_WAIT_SEC 10
#define NCELL_MEAS_CNT 17

const char * const FOTA_STATUS_DETAILS_TIMEOUT = "Download did not complete in the alloted time";
const char * const FOTA_STATUS_DETAILS_DL_ERR  = "Error occured while downloading the file";
const char * const FOTA_STATUS_DETAILS_MDM_REJ = "Modem rejected the update; invalid delta?";
const char * const FOTA_STATUS_DETAILS_MCU_REJ = "Device rejected the update";
const char * const FOTA_STATUS_DETAILS_SUCCESS = "FOTA update completed successfully";

static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];

static K_SEM_DEFINE(fota_download_sem, 0, 1);
static K_SEM_DEFINE(button_press_sem, 0, 1);
static K_SEM_DEFINE(lte_connected, 0, 1);
static K_SEM_DEFINE(cell_data_ready, 0, 1);

static struct lte_lc_ncell neighbor_cells[NCELL_MEAS_CNT];
static struct lte_lc_cells_info cell_data = {
	.neighbor_cells = neighbor_cells,
};
static atomic_t waiting_for_ncell;

static enum nrf_cloud_fota_status fota_status = NRF_CLOUD_FOTA_CANCELED;
static struct nrf_cloud_fota_job_info job;

static const struct device *gps_dev;
static struct gps_config gps_cfg = {
	.nav_mode = GPS_NAV_MODE_PERIODIC,
	.power_mode = GPS_POWER_MODE_DISABLED,
	.timeout = 120,
	.interval = 240,
	.priority = true,
	};

#if defined(CONFIG_REST_DO_JITP)
static bool jitp_requested;
#endif

static char const * fota_status_details = FOTA_STATUS_DETAILS_SUCCESS;
static struct nrf_modem_gnss_pvt_data_frame last_pvt;

static int set_device_id(void)
{
	int err = 0;

#if defined(CONFIG_REST_ID_SRC_COMPILE_TIME)

	memcpy(device_id, CONFIG_REST_DEVICE_ID, strlen(CONFIG_REST_DEVICE_ID));
	return 0;

#elif defined(CONFIG_REST_ID_SRC_IMEI)

	char imei_buf[CGSN_RSP_LEN + 1];
	err = at_cmd_write("AT+CGSN", imei_buf, sizeof(imei_buf), NULL);
	if (err) {
		LOG_ERR("Failed to obtain IMEI, error: %d", err);
		return err;
	}

	imei_buf[IMEI_LEN] = 0;

	err = snprintk(device_id, sizeof(device_id), "%s%.*s",
		       CONFIG_REST_ID_PREFIX,
		       IMEI_LEN, imei_buf);
	if (err < 0 || err >= sizeof(device_id)) {
		return -EIO;
	}

	return 0;

#elif defined(CONFIG_REST_ID_SRC_INTERNAL_UUID)

	struct nrf_device_uuid dev_id;

	err = modem_jwt_get_uuids(&dev_id, NULL);
	if (err) {
		LOG_ERR("Failed to get device UUID: %d", err);
		return err;
	}
	memcpy(device_id, dev_id.str, sizeof(dev_id.str));

	return 0;

#endif

	return -ENOTRECOVERABLE;
}

static void print_satellite_stats(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	uint8_t tracked   = 0;
	uint8_t in_fix    = 0;
	uint8_t unhealthy = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i) {
		if (pvt_data->sv[i].sv > 0) {
			tracked++;

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				in_fix++;
			}

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY) {
				unhealthy++;
			}
		}
	}

	printk("Tracking: %d Using: %d Unhealthy: %d\n", tracked, in_fix, unhealthy);
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	printk("Latitude:   %.06f\n", pvt_data->latitude);
	printk("Longitude:  %.06f\n", pvt_data->longitude);
	printk("Altitude:   %.01f m\n", pvt_data->altitude);
	printk("Accuracy:   %.01f m\n", pvt_data->accuracy);
	printk("Speed:      %.01f m/s\n", pvt_data->speed);
	printk("Heading:    %.01f deg\n", pvt_data->heading);
	printk("Date:       %02u-%02u-%02u\n", pvt_data->datetime.year,
					       pvt_data->datetime.month,
					       pvt_data->datetime.day);
	printk("Time (UTC): %02u:%02u:%02u\n", pvt_data->datetime.hour,
					       pvt_data->datetime.minute,
					       pvt_data->datetime.seconds);
}

static void gps_handler(const struct device *dev, struct gps_event *evt)
{
	ARG_UNUSED(dev);

	switch (evt->type) {
	case GPS_EVT_SEARCH_STARTED:
		LOG_INF("GPS_EVT_SEARCH_STARTED");
		break;
	case GPS_EVT_SEARCH_STOPPED:
		LOG_INF("GPS_EVT_SEARCH_STOPPED");
		break;
	case GPS_EVT_SEARCH_TIMEOUT:
		LOG_INF("GPS_EVT_SEARCH_TIMEOUT");
		break;
	case GPS_EVT_OPERATION_BLOCKED:
		LOG_INF("GPS_EVT_OPERATION_BLOCKED");
		break;
	case GPS_EVT_OPERATION_UNBLOCKED:
		LOG_INF("GPS_EVT_OPERATION_UNBLOCKED");
		break;
	case GPS_EVT_AGPS_DATA_NEEDED:
		LOG_INF("GPS_EVT_AGPS_DATA_NEEDED");
		break;
	case GPS_EVT_PVT:
		LOG_INF("GPS_EVT_PVT");
		nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
		print_satellite_stats(&last_pvt);
		if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			print_fix_data(&last_pvt);
		}
		break;
	case GPS_EVT_PVT_FIX:
		LOG_INF("GPS_EVT_PVT_FIX");
		break;
	case GPS_EVT_NMEA_FIX:
		LOG_INF("GPS_EVT_NMEA_FIX");
		break;
	default:
		break;
	}
}

static void http_fota_handler(const struct fota_download_evt *evt)
{
	LOG_DBG("evt: %d", evt->id);

	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_FINISHED:
		LOG_INF("FOTA complete");
		k_sem_give(&fota_download_sem);
		fota_status = NRF_CLOUD_FOTA_SUCCEEDED;
		break;
	case FOTA_DOWNLOAD_EVT_ERASE_PENDING:
		LOG_INF("FOTA complete");
		fota_status = NRF_CLOUD_FOTA_SUCCEEDED;
		k_sem_give(&fota_download_sem);
		break;
	case FOTA_DOWNLOAD_EVT_ERASE_DONE:
		break;
	case FOTA_DOWNLOAD_EVT_ERROR:
		LOG_INF("FOTA download error: %d", evt->cause);

		fota_status = NRF_CLOUD_FOTA_FAILED;
		fota_status_details = FOTA_STATUS_DETAILS_DL_ERR;

		if (evt->cause == FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE) {
			fota_status = NRF_CLOUD_FOTA_REJECTED;
			if (job.type == NRF_CLOUD_FOTA_MODEM) {
				fota_status_details = FOTA_STATUS_DETAILS_MDM_REJ;
			} else {
				fota_status_details = FOTA_STATUS_DETAILS_MCU_REJ;
			}
		}
		k_sem_give(&fota_download_sem);
		break;
	case FOTA_DOWNLOAD_EVT_PROGRESS:
		LOG_INF("FOTA download percent: %d", evt->progress);
		break;
	default:
		break;
	}
}

int set_led(const int state)
{
#if defined(CONFIG_REST_ENABLE_LED)
#if defined(CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9160NS) || \
    defined(CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9160)
	int err = dk_set_led(LED_NUM, !state);
#else
	int err = dk_set_led(LED_NUM, state);
#endif

	if (err) {
		LOG_ERR("Failed to set LED, error: %d", err);
		return err;
	}
#else
	ARG_UNUSED(state);
#endif
	return 0;
}

int init_led(void)
{
#if defined(CONFIG_REST_ENABLE_LED)
	int err = dk_leds_init();
	if (err)
	{
		LOG_ERR("LED init failed, error: %d", err);
		return err;
	}
	(void)set_led(0);
#endif
	return 0;
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states &
	    BIT(BUTTON_EVENT_BTN_NUM - 1)) {

		LOG_DBG("Button %d pressed", BUTTON_EVENT_BTN_NUM);
		k_sem_give(&button_press_sem);

		if (atomic_get(&waiting_for_ncell)) {
			atomic_set(&waiting_for_ncell, 0);
			k_sem_give(&cell_data_ready);
		}
	}
}

static int request_jitp(void)
{
	int ret = 0;

#if defined(CONFIG_REST_DO_JITP)
	jitp_requested = false;

	(void)k_sem_take(&button_press_sem, K_NO_WAIT);

	LOG_INF("Press button %d to request just-in-time provisioning", BUTTON_EVENT_BTN_NUM);
	LOG_INF("Waiting %d seconds..", JITP_REQ_WAIT_SEC);

	ret = k_sem_take(&button_press_sem, K_SECONDS(JITP_REQ_WAIT_SEC));
	if (ret == -EAGAIN) {
		LOG_INF("JITP will be skipped");
		ret = 0;
	} else if (ret) {
		LOG_ERR("k_sem_take: err %d", ret);
	} else {
		jitp_requested = true;
		LOG_INF("JITP will be performed after network connection is obtained");
		ret = 0;
	}
#endif
	return ret;
}

static int do_jitp(void)
{
	int ret = 0;

#if defined(CONFIG_REST_DO_JITP)
	if (!jitp_requested) {
		return 0;
	}

	LOG_INF("Performing JITP..");
	ret = nrf_cloud_rest_jitp(CONFIG_NRF_CLOUD_SEC_TAG);

	if (ret == 0) {
		LOG_INF("Waiting 30s for cloud provisioning to complete..");
		k_sleep(K_SECONDS(30));
		(void)k_sem_take(&button_press_sem, K_NO_WAIT);
		LOG_INF("Associate device with nRF Cloud account and press button %d when complete",
			BUTTON_EVENT_BTN_NUM);
		(void)k_sem_take(&button_press_sem, K_FOREVER);
	} else if (ret == 1) {
		LOG_INF("Device already provisioned");
	} else {
		LOG_ERR("Device provisioning failed");
	}
#endif
	return ret;
}

int init(void)
{
	int err = init_led();

	if (err){
		LOG_ERR("LED init failed");
		return err;
	}

	err = nrf_modem_lib_init(NORMAL_MODE);
	if (err) {
		LOG_ERR("Failed to initialize modem library: %d", err);
		LOG_ERR("This can occur after a modem FOTA.");
		return err;
	}

	err = at_cmd_init();
	if (err) {
		LOG_ERR("Failed to initialize AT commands, err %d", err);
		return err;
	}

	err = at_notif_init();
	if (err) {
		LOG_ERR("Failed to initialize AT notifications, err %d", err);
		return err;
	}


	gps_dev = device_get_binding("NRF9160_GPS");
	if (gps_dev != NULL) {
		err = gps_init(gps_dev, gps_handler);
		if (!err) {
			LOG_INF("GPS INITD");
		}
	}

	err = set_device_id();
	if (err) {
		LOG_ERR("Failed to set device ID, err %d", err);
		return err;
	}

	LOG_INF("Device ID: %s", log_strdup(device_id));

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Failed to initialize button: err %d", err);
		return err;
	}

	err = request_jitp();
	if (err){
		LOG_WRN("User JITP request failed");
		err = 0;
	}

	return 0;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_DBG("LTE_LC_EVT_NW_REG_STATUS: %d", evt->nw_reg_status);
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		     (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		LOG_INF("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_DBG("LTE cell changed: Cell ID: %d, Tracking area: %d",
			evt->cell.id, evt->cell.tac);
		cell_data.current_cell = evt->cell;
		break;
	case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
		LOG_INF("Neighbor cell measurements received");

		/* Copy current and neighbor cell information. */
		memcpy(&cell_data, &evt->cells_info, sizeof(struct lte_lc_cells_info));
		memcpy(neighbor_cells, evt->cells_info.neighbor_cells,
			sizeof(struct lte_lc_ncell) * cell_data.ncells_count);
		cell_data.neighbor_cells = neighbor_cells;

		k_sem_give(&cell_data_ready);
		break;
	case LTE_LC_EVT_LTE_MODE_UPDATE:
	case LTE_LC_EVT_PSM_UPDATE:
	case LTE_LC_EVT_EDRX_UPDATE:
	case LTE_LC_EVT_RRC_UPDATE:
	default:
		break;
	}
}

static int connect_to_network(void)
{
	int err;

	LOG_INF("Waiting for network..");

	(void)k_sem_take(&lte_connected, K_NO_WAIT);

	err = lte_lc_init_and_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Failed to init modem, err %d", err);
	} else {
		k_sem_take(&lte_connected, K_FOREVER);
		LOG_INF("Connected");
		(void)set_led(1);
	}

	return err;
}


void main(void)
{
	LOG_INF("Starting \"%s\"..", log_strdup(CONFIG_REST_SAMPLE_NAME));

	char rx_buf[REST_RX_BUF_SZ];
	int agps_sz;
	struct nrf_cloud_cell_pos_result location;
	struct jwt_data jwt = {
#if defined(CONFIG_REST_ID_SRC_INTERNAL_UUID)
		.subject = NULL,
#else
		.subject = device_id,
#endif
		.audience = NULL,
		.exp_delta_s = 0,
		.sec_tag = CONFIG_REST_JWT_SEC_TAG,
		.key = JWT_KEY_TYPE_CLIENT_PRIV,
		.alg = JWT_ALG_TYPE_ES256
	};
	struct nrf_cloud_rest_context rest_ctx = {
		.connect_socket = -1,
		.keep_alive = true,
		.timeout_ms = 30000,
		.rx_buf = rx_buf,
		.rx_buf_len = sizeof(rx_buf),
		.fragment_size = 0
	};
	struct nrf_cloud_rest_cell_pos_request loc_req;
	struct nrf_cloud_rest_agps_request agps_req = {
		.type = NRF_CLOUD_REST_AGPS_REQ_CUSTOM
	};
	struct gps_agps_request agps = {
		.utc = 1,
		.sv_mask_ephe = 1,
		.sv_mask_alm = 1,
		.klobuchar = 1
	};
	struct nrf_cloud_rest_agps_result agps_result = {
		.buf_sz = 0,
		.buf = NULL
	};

	struct gps_pgps_request pgps = {
		.gps_day = 15103,
		.prediction_count = 42,
		.prediction_period_min = 420,
		.gps_time_of_day = 0
	};

	struct nrf_cloud_rest_pgps_request pgps_request;

	int err = init();

	if (err)
	{
		LOG_ERR("Initialization failed");
		goto cleanup;
	}

	err = connect_to_network();
	if (err){
		goto cleanup;
	}

	err = gps_start(gps_dev, &gps_cfg);
	if (!err){
		LOG_INF("GPS STARTED!");
	}

	lte_lc_neighbor_cell_measurement();

	(void)do_jitp();

retry:
	err = modem_jwt_generate(&jwt);
	if (err) {
		LOG_ERR("Failed to generate JWT, err %d", err);
		goto cleanup;
	}
	LOG_DBG("JWT:\n%s", log_strdup(jwt.jwt_buf));
	rest_ctx.auth = jwt.jwt_buf;

	if (IS_ENABLED(CONFIG_REST_FOTA_ONLY)) {
		goto fota_start;
	}

	LOG_INF("\n******************* Locate API *******************");
	size_t sem_cnt = k_sem_count_get(&cell_data_ready);
	if (!sem_cnt) {
		k_sem_take(&button_press_sem, K_NO_WAIT);
		atomic_set(&waiting_for_ncell, 1);
		LOG_INF("Waiting for cell data, press button %d to skip..",
			BUTTON_EVENT_BTN_NUM);

		err = k_sem_take(&cell_data_ready, K_FOREVER);

		if (err || !atomic_get(&waiting_for_ncell)) {
			LOG_INF("Skipping locate");
			loc_req.net_info = NULL;
			goto pgps_start;
		}
	}

	loc_req.net_info = &cell_data;
	err = nrf_cloud_rest_cell_pos_get(&rest_ctx, &loc_req, &location);
	if (err) {
		LOG_ERR("Cell Pos API call failed, error: %d", err);
	} else {
		LOG_INF("Cell Pos Response: %s", log_strdup(rest_ctx.response));
	}

pgps_start:
	/* Exercise P-GPS API */
	LOG_INF("\n********************* P-GPS API *********************");

	pgps_request.pgps_req = &pgps;

	err = nrf_cloud_rest_pgps_data_get(&rest_ctx, &pgps_request);
	if (err) {
		LOG_ERR("P-GPS request failed, error: %d", err);
	}

	/* Exercise A-GPS API */
	LOG_INF("\n********************* A-GPS API *********************");

	agps_req.net_info = loc_req.net_info;
	agps_req.agps_req = &agps;
	/* When requesting a potentially large result, set fragment size to a
	 * small value (1) and do not provide a result buffer.
	 * This will allow you to obtain the necessary size while limiting
	 * data transfer.
	 */
	rest_ctx.fragment_size = 1;
	agps_sz = nrf_cloud_rest_agps_data_get(&rest_ctx, &agps_req, NULL);
	if (agps_sz < 0) {
		LOG_ERR("Failed to get A-GPS data: %d", agps_sz);
		goto fota_start;
	} else if (agps_sz == 0) {
		LOG_WRN("A-GPS data size is zero, skipping");
		goto fota_start;
	}

	LOG_INF("Additional buffer required to download A-GPS data of %d bytes",
		agps_sz);

	agps_result.buf_sz = (uint32_t)agps_sz;
	agps_result.buf = k_calloc(agps_result.buf_sz, 1);
	if (!agps_result.buf) {
		LOG_ERR("Failed to allocate %u bytes for A-GPS buffer", agps_result.buf_sz);
		goto fota_start;
	}
	/* Use the default configured fragment size */
	rest_ctx.fragment_size = 0;
	err = nrf_cloud_rest_agps_data_get(&rest_ctx, &agps_req, &agps_result);
	if (err) {
		LOG_ERR("Failed to get A-GPS data: %d", err);
	} else {
		/* Send the A-GPS result buffer to the modem using
		 * gps_process_agps_data() or nrf_cloud_agps_process()
		 */
		LOG_INF("Successfully received A-GPS data");
		agps_cloud_data_process(agps_result.buf, agps_result.agps_sz);
	}

	k_free(agps_result.buf);
	agps_result.buf = NULL;

fota_start:
	LOG_INF("\n******************* FOTA API *******************");

	/* Exercise the FOTA API by checking for a job
	 * and if a job exists, mark it as cancelled.
	 */
	err = nrf_cloud_rest_fota_job_get(&rest_ctx, device_id, &job);
	if (err) {
		LOG_INF("Failed to fetch FOTA job, error: %d", err);
		goto cleanup;
	}

	if (job.type == NRF_CLOUD_FOTA_TYPE__INVALID) {
		LOG_INF("No pending FOTA job");
		goto cleanup;
	}

	LOG_INF("FOTA Job: %s, type: %d\n", log_strdup(job.id), job.type);

	int ret = fota_download_init(http_fota_handler);

	if (ret != 0) {
		LOG_ERR("fota_download_init error: %d", ret);
		return;
	}

	ret = fota_download_start(job.host, job.path,
		CONFIG_REST_JWT_SEC_TAG, NULL, FOTA_DL_FRAGMENT_SZ);

	ret = k_sem_take(&fota_download_sem, K_MINUTES(15));
	if (ret == 0) {
		LOG_INF("FOTA download complete");
		k_sleep(K_SECONDS(5));
	} else if (ret == -EAGAIN) {
		fota_download_cancel();
		LOG_INF("FOTA download timed out");
		fota_status = NRF_CLOUD_FOTA_TIMED_OUT;
		fota_status_details = FOTA_STATUS_DETAILS_TIMEOUT;
	} else {
		goto cleanup;
	}

	/* Disconnect after next API call */
	rest_ctx.keep_alive = false;

	/* Update the job */
	err = nrf_cloud_rest_fota_job_update(&rest_ctx, device_id,
		job.id, fota_status, fota_status_details);

	if (err) {
		LOG_ERR("Failed to update FOTA job, error: %d", err);
	} else {
		LOG_INF("FOTA job updated, status: %d", fota_status);
	}

cleanup:
	(void)nrf_cloud_rest_disconnect(&rest_ctx);
	modem_jwt_free(jwt.jwt_buf);
	jwt.jwt_buf = NULL;
	nrf_cloud_rest_fota_job_free(&job);

	if (err) {
		LOG_INF("Rebooting in 30s..");
		k_sleep(K_SECONDS(30));
	} else if (fota_status == NRF_CLOUD_FOTA_SUCCEEDED) {
		LOG_INF("Rebooting in 30s to complete FOTA update..");
		k_sleep(K_SECONDS(30));
	} else {
		LOG_INF("Retrying in %d minute(s)..",
			CONFIG_REST_RETRY_WAIT_TIME_MIN);
		k_sleep(K_MINUTES(CONFIG_REST_RETRY_WAIT_TIME_MIN));
		goto retry;
	}

	sys_reboot(SYS_REBOOT_COLD);
}
