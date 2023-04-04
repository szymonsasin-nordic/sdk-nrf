/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef COAP_CODEC_H_
#define COAP_CODEC_H_

#include <net/nrf_cloud.h>
#include <net/nrf_cloud_pgps.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_location.h>
#include <modem/lte_lc.h>
#include "coap_client.h"

int coap_codec_encode_message(const char *app_id,
			      const char *str_val, double float_val, int int_val,
			      int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt);

int coap_codec_encode_sensor(const char *app_id, double float_val,
			     int64_t ts, uint8_t *buf, size_t *len,
			     enum coap_content_format fmt);

int coap_codec_encode_pvt(const char *app_id, const struct nrf_cloud_gnss_pvt *pvt,
			  int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt);

int coap_codec_encode_ground_fix_req(struct lte_lc_cells_info const *const cell_info,
				     struct wifi_scan_info const *const wifi_info,
				     uint8_t *buf, size_t *len, enum coap_content_format fmt);

int coap_codec_decode_ground_fix_resp(struct nrf_cloud_location_result *result,
				      const uint8_t *buf, size_t len, enum coap_content_format fmt);

int coap_codec_encode_agps(struct nrf_cloud_rest_agps_request const *const request,
			   uint8_t *buf, size_t *len, bool *query_string,
			   enum coap_content_format fm);

int coap_codec_decode_agps_resp(struct nrf_cloud_rest_agps_result *result,
				const uint8_t *buf, size_t len, enum coap_content_format fmt);

int coap_codec_encode_pgps(struct nrf_cloud_rest_pgps_request const *const request,
			   uint8_t *buf, size_t *len, bool *query_string,
			   enum coap_content_format fmt);

int coap_codec_decode_pgps_resp(struct nrf_cloud_pgps_result *result,
				const uint8_t *buf, size_t len, enum coap_content_format fmt);

int coap_codec_decode_fota_resp(struct nrf_cloud_fota_job_info * job,
				const uint8_t *buf, size_t len, enum coap_content_format fmt);

#endif /* COAP_CODEC_H_ */
