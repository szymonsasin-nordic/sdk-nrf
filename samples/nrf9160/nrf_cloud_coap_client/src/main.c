/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/random/rand32.h>
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <dk_buttons_and_leds.h>
//#include "nrf_cloud_codec.h"
#include "scan_wifi.h"
#include "nrf_cloud_coap.h"
#include "coap_client.h"
#include "coap_codec.h"
#include "dtls.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define CREDS_REQ_WAIT_SEC 10
#define APP_WAIT_CELLS_S 30

#define APP_COAP_JWT_ACK_WAIT_MS 120000
#define APP_COAP_SEND_INTERVAL_MS 10000
#define APP_COAP_CLOSE_THRESHOLD_MS 4000
#define APP_COAP_CONNECTION_CHECK_MS 30000
#define APP_COAP_INTERVAL_LIMIT 60

//#define COAP_POC

/* Uncomment to incrementally increase time between coap packets */
#define DELAY_INTERPACKET_PERIOD

/* Open and close socket every cycle */
/* #define OPEN_AND_SHUT */

#define BTN_NUM 1

/* Modem FW version required to properly run this sample */
#define MFWV_MAJ_SAMPLE_REQ	1
#define MFWV_MIN_SAMPLE_REQ	3
#define MFWV_REV_SAMPLE_REQ	0
/* Modem FW version required for extended neighbor cells search */
#define MFWV_MAJ_EXT_SRCH	1
#define MFWV_MIN_EXT_SRCH	3
#define MFWV_REV_EXT_SRCH	1
/* Modem FW version required for extended GCI neighbor cells search */
#define MFWV_MAJ_EXT_SRCH_GCI	1
#define MFWV_MIN_EXT_SRCH_GCI	3
#define MFWV_REV_EXT_SRCH_GCI	4

static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN];

/* Type of data to be sent in the cellular positioning request */
enum nrf_cloud_location_type active_cell_pos_type = LOCATION_TYPE_SINGLE_CELL;

static bool authorized = false;

/* Search type used for neighbor cell measurements; modem FW version depenedent */
static enum lte_lc_neighbor_search_type search_type =
						  LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_EXTENDED_COMPLETE;

/* Buffer to hold neighbor cell measurement data for multi-cell requests */
static struct lte_lc_ncell neighbor_cells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];

/* Buffer to hold GCI cell measurement data for multi-cell requests */
static struct lte_lc_cell gci_cells[5];

/* Modem info struct used for modem FW version and cell info used for single-cell requests */
static struct modem_param_info mdm_param;

/* Structure to hold all cell info */
static struct lte_lc_cells_info cell_info;

/* Current RRC mode */
static enum lte_lc_rrc_mode cur_rrc_mode = LTE_LC_RRC_MODE_IDLE;

/* Flag to indicate that a neighbor cell measurement should be taken once RRC mode is idle */
static bool request_cells = true;

static uint8_t agps_buf[4096];

/* Semaphore to indicate that cell info has been received */
static K_SEM_DEFINE(cell_info_ready_sem, 0, 1);

/* Mutex for cell info struct */
static K_MUTEX_DEFINE(cell_info_mutex);

/* Semaphore to indicate a button has been pressed */
static K_SEM_DEFINE(button_press_sem, 0, 1);

#if defined(CONFIG_WIFI)
/* Semaphore to indicate Wi-Fi scanning is complete */
static K_SEM_DEFINE(wifi_scan_sem, 0, 1);
#endif

static void get_cell_info(void);

static bool ver_check(int32_t reqd_maj, int32_t reqd_min, int32_t reqd_rev,
		      int32_t maj, int32_t min, int32_t rev)
{
	if (maj > reqd_maj) {
		return true;
	} else if ((maj == reqd_maj) && (min > reqd_min)) {
		return true;
	} else if ((maj == reqd_maj) && (min == reqd_min) && (rev >= reqd_rev)) {
		return true;
	}
	return false;
}

static void check_modem_fw_version(void)
{
	char mfwv_str[128];
	uint32_t major;
	uint32_t minor;
	uint32_t rev;

	if (modem_info_string_get(MODEM_INFO_FW_VERSION, mfwv_str, sizeof(mfwv_str)) <= 0) {
		LOG_WRN("Failed to get modem FW version");
		return;
	}

	LOG_INF("Modem FW version: %s", mfwv_str);

	if (sscanf(mfwv_str, "mfw_nrf9160_%u.%u.%u", &major, &minor, &rev) != 3) {
		LOG_WRN("Unable to parse modem FW version number");
		return;
	}

	/* Ensure the modem firmware version meets the requirement for this sample */
	if (!ver_check(MFWV_MAJ_SAMPLE_REQ, MFWV_MIN_SAMPLE_REQ, MFWV_REV_SAMPLE_REQ,
		       major, minor, rev)) {
		LOG_ERR("This sample requires modem FW version %d.%d.%d or later",
			MFWV_MAJ_SAMPLE_REQ, MFWV_MIN_SAMPLE_REQ, MFWV_REV_SAMPLE_REQ);
		LOG_INF("Update modem firmware and restart");
		k_sleep(K_FOREVER);
	}

	/* Enable GCI/extended search if modem fw version allows */
	if (ver_check(MFWV_MAJ_EXT_SRCH_GCI, MFWV_MIN_EXT_SRCH_GCI, MFWV_REV_EXT_SRCH_GCI,
		      major, minor, rev)) {
		search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_EXTENDED_COMPLETE;
		LOG_INF("Using LTE LC neighbor search type GCI extended complete for %d cells",
			ARRAY_SIZE(gci_cells));
	} else if (ver_check(MFWV_MAJ_EXT_SRCH, MFWV_MIN_EXT_SRCH, MFWV_REV_EXT_SRCH,
			     major, minor, rev)) {
		search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_EXTENDED_COMPLETE;
		LOG_INF("Using LTE LC neighbor search type extended complete");
	} else {
		LOG_INF("Using LTE LC neighbor search type default");
	}
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states & BIT(BTN_NUM - 1)) {
		LOG_DBG("Button %d pressed", BTN_NUM);
		k_sem_give(&button_press_sem);
	}
}

static bool offer_update_creds(void)
{
	int ret;
	bool update_creds = false;

	k_sem_reset(&button_press_sem);

	LOG_INF("---> Press button %d to update credentials in modem", BTN_NUM);
	LOG_INF("     Waiting %d seconds...", CREDS_REQ_WAIT_SEC);

	ret = k_sem_take(&button_press_sem, K_SECONDS(CREDS_REQ_WAIT_SEC));
	if (ret == 0) {
		update_creds = true;
		LOG_INF("Credentials will be updated");
	} else {
		if (ret != -EAGAIN) {
			LOG_ERR("k_sem_take error: %d", ret);
		}

		LOG_INF("Credentials will not be updated");
	}

	return update_creds;
}

#if defined(CONFIG_NRF_MODEM_LIB)
/**@brief Recoverable modem library error. */
void nrf_modem_recoverable_error_handler(uint32_t err)
{
	LOG_ERR("Modem library recoverable error: %u", (unsigned int)err);
}
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);

void lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
	switch (event->type) {
	case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
		LOG_INF("LWM2M_CARRIER_EVENT_BSDLIB_INIT");
		break;
	case LWM2M_CARRIER_EVENT_CONNECT:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECT");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECT:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECT");
		break;
	case LWM2M_CARRIER_EVENT_READY:
		LOG_INF("LWM2M_CARRIER_EVENT_READY");
		k_sem_give(&carrier_registered);
		break;
	case LWM2M_CARRIER_EVENT_FOTA_START:
		LOG_INF("LWM2M_CARRIER_EVENT_FOTA_START");
		break;
	case LWM2M_CARRIER_EVENT_REBOOT:
		LOG_INF("LWM2M_CARRIER_EVENT_REBOOT");
		break;
	}
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE network");
			k_sem_give(&lte_ready);
		} else {
			LOG_INF("reg status %d", evt->nw_reg_status);
		}
		break;

	case LTE_LC_EVT_CELL_UPDATE:
		if (evt->cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID) {
			break;
		}

		/* Get new info when cell ID changes */
		LOG_INF("Cell info changed");
		request_cells = true;
		get_cell_info();
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		cur_rrc_mode = evt->rrc_mode;
		if (cur_rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			LOG_INF("RRC mode: idle");
		} else {
			LOG_INF("RRC mode: connected");
		}
		if (request_cells && (cur_rrc_mode == LTE_LC_RRC_MODE_IDLE)) {
			get_cell_info();
		}
		break;
	case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
		if ((search_type < LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT) &&
		    (evt->cells_info.current_cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID)) {
			LOG_WRN("Current cell ID not valid in neighbor cell measurement results");
			break;
		}

		(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);
		/* Copy current cell information. */
		memcpy(&cell_info.current_cell,
		       &evt->cells_info.current_cell,
		       sizeof(cell_info.current_cell));

		/* Copy neighbor cell information if present. */
		cell_info.ncells_count = evt->cells_info.ncells_count;
		if ((evt->cells_info.ncells_count > 0) && (evt->cells_info.neighbor_cells)) {
			memcpy(neighbor_cells,
			       evt->cells_info.neighbor_cells,
			       sizeof(neighbor_cells[0]) * cell_info.ncells_count);
			LOG_INF("Received measurements for %u neighbor cells",
				cell_info.ncells_count);
		} else {
			LOG_WRN("No neighbor cells were measured");
		}

		/* Copy GCI cell information if present. */
		cell_info.gci_cells_count = evt->cells_info.gci_cells_count;
		if ((evt->cells_info.gci_cells_count > 0) && (evt->cells_info.gci_cells)) {
			memcpy(gci_cells,
			       evt->cells_info.gci_cells,
			       sizeof(gci_cells[0]) * cell_info.gci_cells_count);
			LOG_INF("Received measurements for %u GCI cells",
				cell_info.gci_cells_count);
		} else if (search_type == LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_EXTENDED_COMPLETE) {
			LOG_WRN("No GCI cells were measured");
		}

		(void)k_mutex_unlock(&cell_info_mutex);
		k_sem_give(&cell_info_ready_sem);

		break;
	default:
		LOG_DBG("LTE event %d (0x%x)", evt->type, evt->type);
		break;
	}
}


/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library initialization failed, error: %d", err);
		return;
	}

	lte_lc_register_handler(lte_handler);
#if defined(CONFIG_LTE_LINK_CONTROL)
#if defined(CONFIG_LWM2M_CARRIER)
	/* Wait for the LWM2M_CARRIER to configure the modem and
	 * start the connection.
	 */
	LOG_INF("Waiting for carrier registration...");
	k_sem_take(&carrier_registered, K_FOREVER);
	LOG_INF("Registered!");
#else /* defined(CONFIG_LWM2M_CARRIER) */
	LOG_INF("LTE Link Connecting ...");
	err = lte_lc_init_and_connect();
	__ASSERT(err == 0, "LTE link could not be established.");
	k_sem_take(&lte_ready, K_FOREVER);
	LOG_INF("LTE Link Connected!");
	err = lte_lc_psm_req(true);
	if (err) {
		LOG_ERR("Unable to enter PSM mode: %d", err);
	}

	err = nrf_modem_at_printf("AT+CEREG=5");
	if (err) {
		LOG_ERR("Can't subscribe to +CEREG events.");
	}

#endif /* defined(CONFIG_LWM2M_CARRIER) */
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */
	/* Modem info library is used to obtain the modem FW version
	 * and network info for single-cell requests
	 */
	err = modem_info_init();
	if (err) {
		LOG_ERR("Modem info initialization failed, error: %d", err);
		return;
	}

	err = modem_info_params_init(&mdm_param);
	if (err) {
		LOG_ERR("Modem info params initialization failed, error: %d", err);
		return;
	}

	/* Check modem FW version */
	check_modem_fw_version();
}

static void check_connection(void)
{
#if !defined(DELAY_INTERPACKET_PERIOD) && (APP_COAP_SEND_INTERVAL_MS < APP_COAP_CONNECTION_CHECK_MS)
	return;
#endif

	static int64_t next_keep_serial_alive_time;
	char scratch_buf[256];
	int err;

	if (k_uptime_get() >= next_keep_serial_alive_time) {
		next_keep_serial_alive_time = k_uptime_get() + APP_COAP_CONNECTION_CHECK_MS;
		scratch_buf[0] = '\0';
		err = nrf_modem_at_cmd(scratch_buf, sizeof(scratch_buf), "%s", "AT%XMONITOR");
		LOG_INF("%s", scratch_buf);
		if (err) {
			LOG_ERR("Error on at cmd: %d", err);
		}
	}
}

int init(void)
{
	int err;

	/* Init the button */
	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Failed to initialize button: error: %d", err);
		return err;
	}

	err = client_provision(offer_update_creds());
	if (err) {
		LOG_ERR("Failed to provision credentials: %d", err);
		return err;
	}

	modem_configure();

	err = get_device_id(device_id, sizeof(device_id));
	if (err) {
		LOG_ERR("Error getting device id: %d", err);
		return err;
	}

#if defined(CONFIG_WIFI)
	err = scan_wifi_init();
	if (err) {
		LOG_ERR("Error initializing Wi-Fi scanning: %d", err);
		return err;
	}
#endif

	err = client_init();
	if (err) {
		LOG_ERR("Failed to initialize CoAP client: %d", err);
		return err;
	}

	err = nrf_cloud_coap_init(device_id);
	if (err) {
		LOG_ERR("Failed to initialize nRF Cloud CoAP library: %d", err);
	}
	return err;
}

static void get_cell_info(void)
{
	int err;

	if (!authorized || !request_cells) {
		return;
	}

	struct lte_lc_ncellmeas_params ncellmeas_params = {
		.search_type = search_type,
		.gci_count = ARRAY_SIZE(gci_cells)
	};

	/* Set the result buffers */
	cell_info.neighbor_cells = neighbor_cells;
	cell_info.gci_cells = gci_cells;

	LOG_INF("Requesting neighbor cell measurement");
	err = lte_lc_neighbor_cell_measurement(&ncellmeas_params);
	if (err) {
		LOG_ERR("Failed to start neighbor cell measurement, error: %d", err);
	} else {
		request_cells = false;
		LOG_INF("Waiting for measurement results...");
	}
}

int do_next_test(void)
{
	static double temp = 21.5;
	static int cur_test = 1;
	static struct nrf_cloud_gnss_pvt pvt = {
		.lat = 45.525616, .lon = -122.685978, .accuracy = 30};
	int err = 0;
	struct nrf_cloud_location_result result;
	struct nrf_cloud_fota_job_info job;
	struct nrf_cloud_rest_agps_request agps_request;
	struct nrf_modem_gnss_agps_data_frame agps_req;
	struct nrf_cloud_rest_agps_result agps_res;
	struct nrf_cloud_rest_pgps_request pgps_request;
	struct gps_pgps_request pgps_req;
	struct nrf_cloud_pgps_result pgps_res;
	struct wifi_scan_info *wifi_info = NULL;
	char host[64];
	char path[128];

	switch (cur_test) {
	case 1:
		LOG_INF("******** %d. Sending temperature", cur_test);
		err = nrf_cloud_coap_send_sensor(NRF_CLOUD_JSON_APPID_VAL_TEMP, temp);
		if (err) {
			LOG_ERR("Error sending sensor data: %d", err);
			break;
		}
		temp += 0.1;
		break;
	case 2:
		LOG_INF("******** %d. Getting pending FOTA job execution", cur_test);
		err = nrf_cloud_coap_get_current_fota_job(&job);
		if (err) {
			LOG_ERR("Failed to request pending FOTA job: %d", err);
		} else {
			LOG_INF("******** %d. Updating FOTA job status", cur_test);
			/* process the job */
			err = nrf_cloud_coap_fota_job_update(job.id, NRF_CLOUD_FOTA_REJECTED,
						"Connection to rest of NCS FOTA not yet enabled.");
			if (err) {
				LOG_ERR("Unable to reject job: %d", err);
			} else {
				LOG_WRN("Rejected job because FOTA not hooked up yet.");
			}
		}
		break;
	case 3:
		LOG_INF("******** %d. Getting A-GPS data", cur_test);
		memset(&agps_request, 0, sizeof(agps_request));
		memset(&agps_req, 0, sizeof(agps_req));
		agps_request.type = NRF_CLOUD_REST_AGPS_REQ_ASSISTANCE;
		agps_request.net_info = &cell_info;
		agps_request.agps_req = &agps_req;
		agps_req.data_flags = 0x3f;
		agps_req.sv_mask_alm = 0xffffffff;
		agps_req.sv_mask_ephe = 0xffffffff;
		agps_res.buf = agps_buf;
		agps_res.buf_sz = sizeof(agps_buf);
		err = nrf_cloud_coap_agps(&agps_request, &agps_res);
		if (err) {
			LOG_ERR("Failed to request A-GPS: %d", err);
		} else {
			/* Process the data once it arrives... */
		}
		break;
	case 4:
		LOG_INF("******** %d. Getting P-GPS data", cur_test);
		memset(&pgps_request, 0, sizeof(pgps_request));
		memset(&pgps_res, 0, sizeof(pgps_res));
		pgps_req.gps_day = 0;
		pgps_req.gps_time_of_day = 0;
		pgps_req.prediction_count = 4;
		pgps_req.prediction_period_min = 240;
		pgps_request.pgps_req = &pgps_req;
		pgps_res.host = host;
		pgps_res.host_sz = sizeof(host);
		pgps_res.path = path;
		pgps_res.path_sz = sizeof(path);
		err = nrf_cloud_coap_pgps(&pgps_request, &pgps_res);
		if (err) {
			LOG_ERR("Failed to request P-GPS: %d", err);
		} else {
			LOG_INF("P-GPS host:%s, path:%s", pgps_res.host, pgps_res.path);
		}
		break;
	case 5:
		LOG_INF("******** %d. Getting position", cur_test);
		LOG_INF("Waiting for neighbor cells..");
		err = k_sem_take(&cell_info_ready_sem, K_SECONDS(APP_WAIT_CELLS_S));
		if (err) {
			LOG_ERR("Timeout waiting for cells: %d", err);
			break;
		}
#if defined(CONFIG_WIFI)
		err = scan_wifi_start(&wifi_scan_sem);
		LOG_INF("Waiting for Wi-Fi scans...");
		k_sem_take(&wifi_scan_sem, K_FOREVER);
		if (err) {
			LOG_ERR("Error starting Wi-Fi scan: %d", err);
			break;
		}
		wifi_info = scan_wifi_results_get();
#endif

		(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);

		if (cell_info.current_cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
			LOG_INF("Current cell info: Cell ID: %u, TAC: %u, MCC: %d, MNC: %d",
				cell_info.current_cell.id, cell_info.current_cell.tac,
				cell_info.current_cell.mcc, cell_info.current_cell.mnc);
		} else {
			LOG_WRN("No current serving cell available");
		}

		if (cell_info.ncells_count || cell_info.gci_cells_count) {
			LOG_INF("Performing multi-cell request with "
				"%u neighbor cells and %u GCI cells",
				cell_info.ncells_count, cell_info.gci_cells_count);
		} else {
			LOG_INF("Performing single-cell request");
		}

		err = nrf_cloud_coap_get_location(&cell_info, wifi_info, &result);
		(void)k_mutex_unlock(&cell_info_mutex);
		if (err) {
			LOG_ERR("Unable to get location: %d", err);
			break;
		} else {
			/* Process the returned location once it arrives */
			pvt.lat = result.lat;
			pvt.lon = result.lon;
			pvt.accuracy = result.unc;
		}
		request_cells = true;
		break;
	case 6:
		LOG_INF("******** %d. Sending GNSS PVT", cur_test);
		err = nrf_cloud_coap_send_gnss_pvt(&pvt);
		if (err) {
			LOG_ERR("Error sending GNSS PVT data: %d", err);
			break;
		}
		break;
	}

	if (++cur_test > 6) {
		cur_test = 1;
	}
	return err;
}


void main(void)
{
	int64_t next_msg_time;
	int delta_ms = APP_COAP_SEND_INTERVAL_MS;
	int err;
	int i = 1;
	bool reconnect = false;

	LOG_INF("\n");
	LOG_INF("The nRF Cloud CoAP client sample started\n");

	err = init();
	if (err) {
		LOG_ERR("Halting.");
		for (;;) {
		}
	}
	next_msg_time = k_uptime_get() + delta_ms;

	while (1) {
		if (authorized && (k_uptime_get() >= next_msg_time)) {
			if (reconnect) {
				reconnect = false;
				authorized = false;
				LOG_INF("Going online");
				err = lte_lc_normal();
				if (err) {
					LOG_ERR("Error going online: %d", err);
				} else {
					k_sem_take(&lte_ready, K_FOREVER);
					if (client_init() != 0) {
						LOG_ERR("Failed to initialize CoAP client");
						return;
					}
				}
			}

			err = do_next_test();
			if (err == -EAGAIN) {
				reconnect = true;
				err = client_close();
				if (err) {
					LOG_ERR("Error closing socket: %d", err);
				} else {
					LOG_INF("Socket closed.");
				}
				LOG_INF("Going offline");
				err = lte_lc_offline();
				if (err) {
					LOG_ERR("Error going offline: %d", err);
				} else {
					LOG_INF("Offline.");
				}
				continue;
			}

			delta_ms = APP_COAP_SEND_INTERVAL_MS * i;
			LOG_INF("Next transfer in %d minutes, %d seconds",
				delta_ms / 60000, (delta_ms / 1000) % 60);
			next_msg_time += delta_ms;
#if defined(DELAY_INTERPACKET_PERIOD)
			if (++i > APP_COAP_INTERVAL_LIMIT) {
				i = APP_COAP_INTERVAL_LIMIT;
			}
#endif
#if defined(CONFIG_COAP_DTLS) || defined(CONFIG_NRF_CLOUD_COAP_DTLS)
			dtls_print_connection_id(client_get_sock(), false);
#endif
		}

		if (!err && !authorized) {
			err = client_wait_ack(APP_COAP_JWT_ACK_WAIT_MS);
			if (!err) {
				LOG_INF("Authorization received");
				authorized = true;
				get_cell_info();
			}
		}
	}
	client_close();
}
