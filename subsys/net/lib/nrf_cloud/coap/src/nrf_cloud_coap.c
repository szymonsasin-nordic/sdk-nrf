/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <date_time.h>
#include <dk_buttons_and_leds.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include "nrf_cloud_codec_internal.h"
#include "nrf_cloud_coap.h"
#include "nrf_cloud_mem.h"
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

static uint8_t buffer[500];
static char topic[100];

int nrf_cloud_coap_init(const char *device_id)
{
	snprintf(topic, sizeof(topic) - 1, "d/%s/d2c", device_id);
	return 0;
}

static int64_t get_ts(void)
{
	int64_t ts;
	int err;

	ts = k_uptime_get();
	err = date_time_uptime_to_unix_time_ms(&ts);
	if (err) {
		LOG_ERR("Error converting time: %d", err);
		ts = 0;
	}
	return ts;
}

#if defined(CONFIG_NRF_CLOUD_AGPS)
#define AGPS_WAIT_MS 20000
static K_SEM_DEFINE(agps_sem, 0, 1);
static int agps_err;

static int get_agps(const void *buf, size_t len, enum coap_content_format fmt, void *user)
{
	agps_err = coap_codec_decode_agps_resp(user, buf, len, fmt);
	k_sem_give(&agps_sem);
	return agps_err;
}

int nrf_cloud_coap_agps(struct nrf_cloud_rest_agps_request const *const request,
			struct nrf_cloud_rest_agps_result *result)
{
	size_t len = sizeof(buffer);
	bool query_string;
	int err;

	err = coap_codec_encode_agps(request, buffer, &len, &query_string,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode A-GPS request: %d", err);
		return err;
	}
	if (query_string) {
		err = client_get_send("loc/agps", (const char *)buffer,
				      NULL, 0, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_agps, result);
	} else {
		err = client_fetch_send("loc/agps", NULL,
				      buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_agps, result);
	}

	if (err) {
		LOG_ERR("Failed to send GET request: %d", err);
		return err;
	}

	LOG_INF("Waiting for response...");
	err = k_sem_take(&agps_sem, K_MSEC(AGPS_WAIT_MS));
	if (!err && !agps_err) {
		LOG_INF("Got A-GPS data");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for A-GPS data");
	} else {
		LOG_ERR("Error getting A-GPS; agps_err:%d, err:%d", agps_err, err);
		err = agps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_AGPS */

#if defined(CONFIG_NRF_CLOUD_PGPS)
#define PGPS_WAIT_MS 20000
static K_SEM_DEFINE(pgps_sem, 0, 1);
static int pgps_err;

static int get_pgps(const void *buf, size_t len, enum coap_content_format fmt, void *user)
{
	pgps_err = coap_codec_decode_pgps_resp(user, buf, len, fmt);
	k_sem_give(&pgps_sem);
	return pgps_err;
}

int nrf_cloud_coap_pgps(struct nrf_cloud_rest_pgps_request const *const request,
			struct nrf_cloud_pgps_result *result)
{
	size_t len = sizeof(buffer);
	bool query_string;
	int err;

	err = coap_codec_encode_pgps(request, buffer, &len, &query_string,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode P-GPS request: %d", err);
		return err;
	}
	if (query_string) {
		err = client_get_send("loc/pgps", (const char *)buffer,
				      NULL, 0, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_JSON, get_pgps, result);
	} else {
		err = client_get_send("loc/pgps", NULL,
				      buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_JSON, get_pgps, result);
	}

	if (err) {
		LOG_ERR("Failed to send GET request: %d", err);
		return err;
	}

	LOG_INF("Waiting for response...");
	err = k_sem_take(&pgps_sem, K_MSEC(PGPS_WAIT_MS));
	if (!err && !pgps_err) {
		LOG_INF("Got P-GPS data");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for P-GPS data");
	} else {
		LOG_ERR("Error getting P-GPS; pgps_err:%d, err:%d", pgps_err, err);
		err = pgps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_PGPS */

int nrf_cloud_coap_send_sensor(const char *app_id, double value)
{
	int64_t ts = get_ts();
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_sensor(app_id, value, ts, buffer, &len,
				       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode sensor data: %d", err);
		return err;
	}
	err = client_post_send("msg", NULL, buffer, len,
			       COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

int nrf_cloud_coap_send_gnss_pvt(const struct nrf_cloud_gnss_pvt *pvt)
{
	int64_t ts = get_ts();
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_pvt("GNSS", pvt, ts, buffer, &len,
				    COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode GNSS PVT data: %d", err);
		return err;
	}
	err = client_post_send("msg", NULL, buffer, len,
			       COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

#define LOC_WAIT_MS 20000
static K_SEM_DEFINE(loc_sem, 0, 1);
static int loc_err;

static int get_location(const void *buf, size_t len, enum coap_content_format fmt, void *user)
{
	loc_err = coap_codec_decode_ground_fix_resp(user, buf, len, fmt);
	k_sem_give(&loc_sem);
	return loc_err;
}

int nrf_cloud_coap_get_location(struct lte_lc_cells_info const *const cell_info,
				struct wifi_scan_info const *const wifi_info,
				struct nrf_cloud_location_result *const result)
{
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_ground_fix_req(cell_info, wifi_info, buffer, &len,
					       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode cell pos data: %d", err);
		return err;
	}
	err = client_fetch_send("loc/ground-fix", NULL, buffer, len,
				COAP_CONTENT_FORMAT_APP_CBOR,
				COAP_CONTENT_FORMAT_APP_CBOR, get_location, result);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
		return err;
	}

	LOG_INF("Waiting for response...");
	err = k_sem_take(&loc_sem, K_MSEC(LOC_WAIT_MS));
	if (!err && !loc_err) {
		LOG_INF("Location: %d, %f, %f, %d", result->type,
			result->lat, result->lon, result->unc);
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for location");
	} else {
		LOG_ERR("Error getting location; loc_err:%d, err:%d", loc_err, err);
		err = loc_err;
	}

	return err;
}


#define FOTA_WAIT_MS 20000
static K_SEM_DEFINE(fota_sem, 0, 1);
static int fota_err;

static int get_fota(const void *buf, size_t len, enum coap_content_format fmt, void *user)
{
	LOG_INF("Got FOTA response: %.*s", len, (const char *)buf);
	fota_err = coap_codec_decode_fota_resp(user, buf, len, fmt);
	k_sem_give(&fota_sem);
	return fota_err;
}

int nrf_cloud_coap_get_current_fota_job(struct nrf_cloud_fota_job_info *const job)
{
	int err;

	err = client_get_send("fota/exec/current", NULL, NULL, 0,
			      COAP_CONTENT_FORMAT_APP_CBOR,
			      COAP_CONTENT_FORMAT_APP_JSON, get_fota, job);
	if (err) {
		LOG_ERR("Failed to send GET request: %d", err);
		return err;
	}

	LOG_INF("Waiting for response...");
	err = k_sem_take(&fota_sem, K_MSEC(FOTA_WAIT_MS));
	if (!err && !fota_err) {
		LOG_INF("FOTA job received; type:%d, id:%s, host:%s, path:%s, size:%d",
			job->type, job->id, job->host, job->path, job->file_size);
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for FOTA job");
	} else {
		LOG_ERR("Error getting current FOTA job; FOTA err:%d, err:%d",
			fota_err, err);
		err = fota_err;
	}
	return err;
}


int nrf_cloud_coap_fota_job_update(const char *const job_id,
	const enum nrf_cloud_fota_status status, const char * const details)
{
	__ASSERT_NO_MSG(device_id != NULL);
	__ASSERT_NO_MSG(job_id != NULL);
	__ASSERT_NO_MSG(status < JOB_STATUS_STRING_COUNT);
#define API_FOTA_JOB_EXEC		"fota/exec"
#define API_UPDATE_FOTA_URL_TEMPLATE	(API_FOTA_JOB_EXEC "/%s")
#define API_UPDATE_FOTA_BODY_TEMPLATE	"{\"status\":\"%s\"}"
#define API_UPDATE_FOTA_DETAILS_TMPLT	"{\"status\":\"%s\", \"details\":\"%s\"}"
	/* Mapping of enum to strings for Job Execution Status. */
	static const char *const job_status_strings[] = {
		[NRF_CLOUD_FOTA_QUEUED]      = "QUEUED",
		[NRF_CLOUD_FOTA_IN_PROGRESS] = "IN_PROGRESS",
		[NRF_CLOUD_FOTA_FAILED]      = "FAILED",
		[NRF_CLOUD_FOTA_SUCCEEDED]   = "SUCCEEDED",
		[NRF_CLOUD_FOTA_TIMED_OUT]   = "TIMED_OUT",
		[NRF_CLOUD_FOTA_REJECTED]    = "REJECTED",
		[NRF_CLOUD_FOTA_CANCELED]    = "CANCELLED",
		[NRF_CLOUD_FOTA_DOWNLOADING] = "DOWNLOADING",
	};
#define JOB_STATUS_STRING_COUNT (sizeof(job_status_strings) / \
				 sizeof(*job_status_strings))

	int ret;
	size_t buff_sz;
	char *url = NULL;
	char *payload = NULL;

	/* Format API URL with device and job ID */
	buff_sz = sizeof(API_UPDATE_FOTA_URL_TEMPLATE) + strlen(job_id);
	url = nrf_cloud_malloc(buff_sz);
	if (!url) {
		ret = -ENOMEM;
		goto clean_up;
	}

	ret = snprintk(url, buff_sz, API_UPDATE_FOTA_URL_TEMPLATE, job_id);
	if ((ret < 0) || (ret >= buff_sz)) {
		LOG_ERR("Could not format URL");
		ret = -ETXTBSY;
		goto clean_up;
	}

	/* Format payload */
	if (details) {
		buff_sz = sizeof(API_UPDATE_FOTA_DETAILS_TMPLT) +
			  strlen(job_status_strings[status]) +
			  strlen(details);
	} else {
		buff_sz = sizeof(API_UPDATE_FOTA_BODY_TEMPLATE) +
			  strlen(job_status_strings[status]);
	}

	payload = nrf_cloud_malloc(buff_sz);
	if (!payload) {
		ret = -ENOMEM;
		goto clean_up;
	}

	if (details) {
		ret = snprintk(payload, buff_sz, API_UPDATE_FOTA_DETAILS_TMPLT,
			       job_status_strings[status], details);
	} else {
		ret = snprintk(payload, buff_sz, API_UPDATE_FOTA_BODY_TEMPLATE,
			       job_status_strings[status]);
	}
	if ((ret < 0) || (ret >= buff_sz)) {
		LOG_ERR("Could not format payload");
		ret = -ETXTBSY;
		goto clean_up;
	}

	ret = client_patch_send(url, NULL, payload, ret,
				COAP_CONTENT_FORMAT_APP_JSON, NULL, NULL);

clean_up:
	if (url) {
		nrf_cloud_free(url);
	}
	if (payload) {
		nrf_cloud_free(payload);
	}

	return ret;
}

