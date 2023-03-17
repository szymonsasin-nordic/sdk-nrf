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
#include "nrf_cloud_coap.h"
#include "coap_client.h"
#include "coap_codec.h"
#include "dtls.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define CREDS_REQ_WAIT_SEC 10

//#define COAP_POC

/* Uncomment to incrementally increase time between coap packets */
/* #define DELAY_INTERPACKET_PERIOD */

/* Open and close socket every cycle */
/* #define OPEN_AND_SHUT */

#define BTN_NUM 1

static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN];

/* Type of data to be sent in the cellular positioning request */
enum nrf_cloud_location_type active_cell_pos_type = LOCATION_TYPE_SINGLE_CELL;

static bool authorized = false;

#if 0
/* Search type used for neighbor cell measurements; modem FW version depenedent */
static enum lte_lc_neighbor_search_type search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_DEFAULT;

/* Buffer to hold neighbor cell measurement data for multi-cell requests */
static struct lte_lc_ncell neighbor_cells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];

/* Buffer to hold GCI cell measurement data for multi-cell requests */
static struct lte_lc_cell gci_cells[CONFIG_REST_CELL_GCI_COUNT];
#endif

/* Modem info struct used for modem FW version and cell info used for single-cell requests */
static struct modem_param_info mdm_param;

/* Structure to hold all cell info */
static struct lte_lc_cells_info cell_info;

static uint8_t agps_buf[4096];

/* Semaphore to indicate that cell info has been received */
static K_SEM_DEFINE(cell_info_ready_sem, 0, 1);

/* Mutex for cell info struct */
static K_MUTEX_DEFINE(cell_info_mutex);

/* Semaphore to indicate a button has been pressed */
static K_SEM_DEFINE(button_press_sem, 0, 1);

static void get_cell_info(void);

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
		get_cell_info();
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

	lte_lc_register_handler(lte_handler);
#if defined(CONFIG_LTE_LINK_CONTROL)
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
#if defined(CONFIG_LWM2M_CARRIER)
		/* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
		LOG_INF("Waiting for carrier registration...");
		k_sem_take(&carrier_registered, K_FOREVER);
		LOG_INF("Registered!");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

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
	}
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

	if (!authorized) {
		return;
	}

	LOG_INF("Getting current cell info...");

	/* Use the modem info library to easily obtain the required network info
	 * for a single-cell request without performing a neighbor cell measurement
	 */
	err = modem_info_params_get(&mdm_param);
	if (err) {
		LOG_ERR("Unable to obtain modem info, error: %d", err);
		return;
	}

	(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);
	memset(&cell_info, 0, sizeof(cell_info));
	/* Required parameters */
	cell_info.current_cell.id	= (uint32_t)mdm_param.network.cellid_dec;
	cell_info.current_cell.mcc	= mdm_param.network.mcc.value;
	cell_info.current_cell.mnc	= mdm_param.network.mnc.value;
	cell_info.current_cell.tac	= mdm_param.network.area_code.value;
	/* Optional */
	cell_info.current_cell.rsrp	= mdm_param.network.rsrp.value;
	/* Omitted - optional parameters not available from modem_info */
	cell_info.current_cell.timing_advance	= NRF_CLOUD_LOCATION_CELL_OMIT_TIME_ADV;
	cell_info.current_cell.rsrq		= NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ;
	cell_info.current_cell.earfcn		= NRF_CLOUD_LOCATION_CELL_OMIT_EARFCN;
	(void)k_mutex_unlock(&cell_info_mutex);

	k_sem_give(&cell_info_ready_sem);
}

int do_next_test(void)
{
	static double temp = 21.5;
	static int post_count = 1;
	static int get_count = 1;
	static int cur_test = 1;
	int err = 0;
	struct nrf_cloud_location_result result;
	struct nrf_cloud_fota_job_info job;
	struct nrf_cloud_rest_agps_request agps_request;
	struct nrf_modem_gnss_agps_data_frame agps_req;
	struct nrf_cloud_rest_agps_result agps_res;

	switch (cur_test) {
	case 1:
		LOG_INF("******** %d. Sending temperature", post_count++);
		err = nrf_cloud_coap_send_sensor(NRF_CLOUD_JSON_APPID_VAL_TEMP, temp);
		if (err) {
			LOG_ERR("Error sending sensor data: %d", err);
			break;
		}
		temp += 0.1;
		break;
	case 2:
		LOG_INF("******** %d. Getting cell position", post_count++);
		err = nrf_cloud_coap_get_location(&cell_info, NULL, &result);
		if (err) {
			LOG_ERR("Unable to get location: %d", err);
			break;
		} else {
			/* Process the returned location once it arrives */
		}
		break;
	case 3:
		LOG_INF("******** %d. Getting pending FOTA job execution", get_count++);
		err = nrf_cloud_get_current_fota_job(&job);
		if (err) {
			LOG_ERR("Failed to request pending FOTA job: %d", err);
		} else {
			/* process the job */
		}
		break;
	case 4:
		LOG_INF("******** %d. Getting A-GPS data", get_count++);
		memset(&agps_request, 0, sizeof(agps_request));
		memset(&agps_req, 0, sizeof(agps_req));
		agps_request.type = NRF_CLOUD_REST_AGPS_REQ_ASSISTANCE; // NRF_CLOUD_REST_AGPS_REQ_CUSTOM;
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
	}

	if (++cur_test > 4) {
		cur_test = 1;
	}
	LOG_DBG("Posts: %d, Gets: %d\n", post_count, get_count);
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

			delta_ms = APP_COAP_SEND_INTERVAL_MS * i;
			LOG_INF("Next transfer in %d minutes, %d seconds",
				delta_ms / 60000, (delta_ms / 1000) % 60);
			next_msg_time += delta_ms;
#if defined(DELAY_INTERPACKET_PERIOD)
			if (++i > APP_COAP_INTERVAL_LIMIT) {
				i = APP_COAP_INTERVAL_LIMIT;
			}
#endif
#if defined(CONFIG_COAP_DTLS)
			dtls_print_connection_id(client_get_sock(), false);
#endif
		}

		if (reconnect) {
			k_sleep(K_MSEC(APP_COAP_RECEIVE_INTERVAL_MS));
			check_connection();
			continue;
		}

		if (!err && !authorized) {
			err = client_wait_ack(APP_COAP_JWT_ACK_WAIT_MS);
			if (!err) {
				LOG_INF("Authorization received");
				authorized = true;
				get_cell_info();
			}
		}
#if defined(OPEN_AND_SHUT)
		if (delta_ms > APP_COAP_CLOSE_THRESHOLD_MS) {
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
			}
		}
#endif
	}
	client_close();
}
