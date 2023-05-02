/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <modem/lte_lc.h>
#include <net/wifi_location_common.h>
#include <cJSON.h>
#include "nrf_cloud_codec_internal.h"
#include "ground_fix_encode_types.h"
#include "ground_fix_encode.h"
#include "ground_fix_decode_types.h"
#include "ground_fix_decode.h"
#include "agps_encode_types.h"
#include "agps_encode.h"
#include "pgps_encode_types.h"
#include "pgps_encode.h"
#include "pgps_decode_types.h"
#include "pgps_decode.h"
#include "msg_encode_types.h"
#include "msg_encode.h"
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_codec, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

#define AGPS_FILTERED			"filtered=true"
#define AGPS_ELEVATION_MASK		"&mask=%u"
#define AGPS_NET_INFO			"&mcc=%u&mnc=%u&tac=%u&eci=%u"
#define AGPS_CUSTOM_TYPE		"&types=%s"
#define AGPS_REQ_TYPE_STR_CUSTOM	"custom"
#define AGPS_REQ_TYPE_STR_LOC		"rtLocation"
#define AGPS_REQ_TYPE_STR_ASSIST	"rtAssistance"
#define PGPS_REQ_PREDICT_CNT		"&" NRF_CLOUD_JSON_PGPS_PRED_COUNT "=%u"
#define PGPS_REQ_PREDICT_INT_MIN	"&" NRF_CLOUD_JSON_PGPS_INT_MIN "=%u"
#define PGPS_REQ_START_GPS_DAY		"&" NRF_CLOUD_JSON_PGPS_GPS_DAY "=%u"
#define PGPS_REQ_START_GPS_TOD_S	"&" NRF_CLOUD_JSON_PGPS_GPS_TIME "=%u"

static int encode_message(const char *app_id, const char *str,
			  const struct nrf_cloud_gnss_pvt *pvt,
			  double float_val, int int_val,
			  int64_t ts, uint8_t *buf, size_t *len,
			  enum coap_content_format fmt)
{
	int err;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		struct message_out input;
		size_t out_len;

		memset(&input, 0, sizeof(struct message_out));
		input._message_out_appId.value = app_id;
		input._message_out_appId.len = strlen(app_id);
		if (str) {
			input._message_out_data_choice = _message_out_data_tstr;
			input._message_out_data_tstr.value = str;
			input._message_out_data_tstr.len = strlen(str);
		} else if (pvt) {
			input._message_out_data_choice = _message_out_data__pvt;
			input._message_out_data__pvt._pvt_lat = pvt->lat;
			input._message_out_data__pvt._pvt_lng = pvt->lon;
			input._message_out_data__pvt._pvt_acc = pvt->accuracy;
			if (pvt->has_speed) {
				input._message_out_data__pvt._pvt_spd._pvt_spd = pvt->speed;
				input._message_out_data__pvt._pvt_spd_present = true;
			}
			if (pvt->has_heading) {
				input._message_out_data__pvt._pvt_hdg._pvt_hdg = pvt->heading;
				input._message_out_data__pvt._pvt_hdg_present = true;
			}
			if (pvt->has_alt) {
				input._message_out_data__pvt._pvt_alt._pvt_alt = pvt->alt;
				input._message_out_data__pvt._pvt_alt_present = true;
			}
		} else if (!isnan(float_val)) {
			input._message_out_data_choice = _message_out_data_float;
			input._message_out_data_float = float_val;
		} else {
			input._message_out_data_choice = _message_out_data_int;
			input._message_out_data_int = int_val;
		}
		input._message_out_ts._message_out_ts = ts;
		input._message_out_ts_present = true;
		err = cbor_encode_message_out(buf, *len, &input, &out_len);
		if (err) {
			LOG_ERR("Error %d encoding message", err);
			err = -EINVAL;
			*len = 0;
		} else {
			*len = out_len;
		}
	} else {
		struct nrf_cloud_data out;

		err = nrf_cloud_encode_message(app_id, float_val, str, NULL, ts, &out);
		if (err) {
			*len = 0;
		} else if (*len < out.len) {
			*len = 0;
			cJSON_free((void *)out.ptr);
			err = -E2BIG;
		} else {
			*len = out.len;
			memcpy(buf, out.ptr, *len);
			buf[*len] = '\0';
			cJSON_free((void *)out.ptr);
		}
	}
	return err;
}

int coap_codec_encode_message(const char *app_id,
			      const char *str_val, double float_val, int int_val,
			      int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	return encode_message(app_id, str_val, NULL, float_val, int_val, ts, buf, len, fmt);
}

int coap_codec_encode_sensor(const char *app_id, double float_val,
			     int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	return encode_message(app_id, NULL, NULL, float_val, 0, ts, buf, len, fmt);
}

int coap_codec_encode_pvt(const char *app_id, const struct nrf_cloud_gnss_pvt *pvt,
			  int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	return encode_message(app_id, NULL, pvt, 0, 0, ts, buf, len, fmt);
}

static void copy_cell(struct cell *dst, struct lte_lc_cell const *const src)
{
	dst->_cell_mcc = src->mcc;
	dst->_cell_mnc = src->mnc;
	dst->_cell_eci = src->id;
	dst->_cell_tac = src->tac;
	if (src->earfcn != NRF_CLOUD_LOCATION_CELL_OMIT_EARFCN) {
		dst->_cell_earfcn._cell_earfcn = src->earfcn;
		dst->_cell_earfcn_present = true;
	} else {
		dst->_cell_earfcn_present = false;
	}
	if (src->timing_advance != NRF_CLOUD_LOCATION_CELL_OMIT_TIME_ADV) {
		uint16_t t_adv = src->timing_advance;

		if (t_adv > NRF_CLOUD_LOCATION_CELL_TIME_ADV_MAX) {
			t_adv = NRF_CLOUD_LOCATION_CELL_TIME_ADV_MAX;
		}
		dst->_cell_adv._cell_adv = t_adv;
		dst->_cell_adv_present = true;
	} else {
		dst->_cell_adv_present = false;
	}
	if (src->rsrp != NRF_CLOUD_LOCATION_CELL_OMIT_RSRP) {
		dst->_cell_rsrp._cell_rsrp = RSRP_IDX_TO_DBM(src->rsrp);
		dst->_cell_rsrp_present = true;
	} else {
		dst->_cell_rsrp_present = false;
	}
	if (src->rsrq != NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ) {
		dst->_cell_rsrq._cell_rsrq_float32 = RSRQ_IDX_TO_DB(src->rsrq);
		dst->_cell_rsrq._cell_rsrq_choice = _cell_rsrq_float32;
		dst->_cell_rsrq_present = true;
	} else {
		dst->_cell_rsrq_present = false;
	}
}

static void copy_ncells(struct ncell *dst, int num, struct lte_lc_ncell *src)
{
	for (int i = 0; i < num; i++) {
		dst->_ncell_earfcn = src->earfcn;
		dst->_ncell_pci = src->phys_cell_id;
		if (src->rsrp != NRF_CLOUD_LOCATION_CELL_OMIT_RSRP) {
			dst->_ncell_rsrp._ncell_rsrp = RSRP_IDX_TO_DBM(src->rsrp);
			dst->_ncell_rsrp_present = true;
		} else {
			dst->_ncell_rsrp_present = false;
		}
		if (src->rsrq != NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ) {
			dst->_ncell_rsrq._ncell_rsrq_float32 = RSRQ_IDX_TO_DB(src->rsrq);
			dst->_ncell_rsrq._ncell_rsrq_choice = _ncell_rsrq_float32;
			dst->_ncell_rsrq_present = true;
		} else {
			dst->_ncell_rsrq_present = false;
		}
		if (src->time_diff != LTE_LC_CELL_TIME_DIFF_INVALID) {
			dst->_ncell_timeDiff._ncell_timeDiff = src->time_diff;
			dst->_ncell_timeDiff_present = true;
		} else {
			dst->_ncell_timeDiff_present = false;
		}
		src++;
		dst++;
	}
}

static void copy_cell_info(struct lte_ar *lte_encode,
			   struct lte_lc_cells_info const *const cell_info)
{
	if (cell_info != NULL) {
		struct cell *cell = lte_encode->_lte_ar__cell;
		size_t num_cells = ARRAY_SIZE(lte_encode->_lte_ar__cell);

		if (cell_info->current_cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
			lte_encode->_lte_ar__cell_count++;
			copy_cell(cell, &cell_info->current_cell);
			cell->_cell_nmr._cell_nmr_ncells_count = cell_info->ncells_count;
			if (cell_info->ncells_count) {
				copy_ncells(cell->_cell_nmr._cell_nmr_ncells,
					    MIN(ARRAY_SIZE(cell->_cell_nmr._cell_nmr_ncells),
						cell_info->ncells_count),
					    cell_info->neighbor_cells);
			}
			cell++;
			num_cells--;
		}
		if (cell_info->gci_cells != NULL) {
			for (int i = 0; i < cell_info->gci_cells_count; i++) {
				lte_encode->_lte_ar__cell_count++;
				copy_cell(cell, &cell_info->gci_cells[i]);
				cell++;
				num_cells--;
				if (!num_cells) {
					break;
				}
			}
		}
	}
}

static void copy_wifi_info(struct wifi_ob *wifi_encode,
			   struct wifi_scan_info const *const wifi_info)
{
	struct ap *dst = wifi_encode->_wifi_ob_accessPoints__ap;
	struct wifi_scan_result *src = wifi_info->ap_info;
	size_t num_aps = ARRAY_SIZE(wifi_encode->_wifi_ob_accessPoints__ap);

	wifi_encode->_wifi_ob_accessPoints__ap_count = 0;

	for (int i = 0; i < wifi_info->cnt; i++) {
		wifi_encode->_wifi_ob_accessPoints__ap_count++;

		dst->_ap_macAddress.value = src->mac;
		dst->_ap_macAddress.len = src->mac_length;
		dst->_ap_age_present = false;
		if (src->rssi != NRF_CLOUD_LOCATION_WIFI_OMIT_RSSI) {
			dst->_ap_signalStrength._ap_signalStrength = src->rssi;
			dst->_ap_signalStrength_present = true;
		} else {
			dst->_ap_signalStrength_present = false;
		}
		if (src->channel != NRF_CLOUD_LOCATION_WIFI_OMIT_CHAN) {
			dst->_ap_channel._ap_channel = src->channel;
			dst->_ap_channel_present = true;
		} else {
			dst->_ap_channel_present = false;
		}
		dst->_ap_frequency_present = false;
		if (src->ssid_length && src->ssid[0]) {
			dst->_ap_ssid._ap_ssid.value = src->ssid;
			dst->_ap_ssid._ap_ssid.len = src->ssid_length;
			dst->_ap_ssid_present = true;
		} else {
			dst->_ap_ssid_present = false;
		}
		num_aps--;
		if (!num_aps) {
			break;
		}
	}
}

int coap_codec_encode_ground_fix_req(struct lte_lc_cells_info const *const cell_info,
				     struct wifi_scan_info const *const wifi_info,
				     uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	int err = 0;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		struct ground_fix_req input;
		size_t out_len;

		memset(&input, 0, sizeof(struct ground_fix_req));
		input._ground_fix_req_lte_present = (cell_info != NULL);
		if (cell_info) {
			copy_cell_info(&input._ground_fix_req_lte._ground_fix_req_lte, cell_info);
		}
		input._ground_fix_req_wifi_present = (wifi_info != NULL);
		if (wifi_info) {
			copy_wifi_info(&input._ground_fix_req_wifi._ground_fix_req_wifi, wifi_info);
		}
		err = cbor_encode_ground_fix_req(buf, *len, &input, &out_len);
		if (err) {
			LOG_ERR("Error %d encoding groundfix", err);
			err = -EINVAL;
			*len = 0;
		} else {
			*len = out_len;
		}
	} else {
		cJSON *obj;
		bool request_loc = true;

		obj = json_create_req_obj(NRF_CLOUD_JSON_APPID_VAL_LOCATION,
					  NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA);
		cJSON *data_obj = cJSON_AddObjectToObject(obj, NRF_CLOUD_JSON_DATA_KEY);

		if (!data_obj) {
			err = -ENOMEM;
			goto cleanup;
		}

		if (cell_info) {
			err = nrf_cloud_cell_pos_req_json_encode(cell_info, data_obj);
			if (err) {
				LOG_ERR("Failed to add cell info to location request, error: %d", err);
				goto cleanup;
			}
		}

		if (wifi_info) {
			err = nrf_cloud_wifi_req_json_encode(wifi_info, data_obj);
			if (err) {
				LOG_ERR("Failed to add WiFi info to location request, error: %d", err);
				goto cleanup;
			}
		}

		/* By default, nRF Cloud will send the location to the device */
		if (!request_loc &&
		    !cJSON_AddNumberToObjectCS(data_obj, NRF_CLOUD_LOCATION_KEY_DOREPLY, 0)) {
			err = -ENOMEM;
			goto cleanup;
		}

		if (!err) {
			cJSON_PrintPreallocated(obj, buf, *len, false);
			*len = strlen((const char *)buf);
		}
cleanup:
		cJSON_Delete(obj);
	}
	return err;
}

int coap_codec_decode_ground_fix_resp(struct nrf_cloud_location_result *result,
				      const uint8_t *buf, size_t len, enum coap_content_format fmt)
{
	int err;

	if (!result) {
		return -EINVAL;
	}
	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		struct ground_fix_resp res;
		size_t out_len;
		int type;

		err = cbor_decode_ground_fix_resp(buf, len, &res, &out_len);
		if (out_len != len) {
			LOG_WRN("Different response length: expected:%zd, decoded:%zd",
				len, out_len);
		}
		if (err) {
			return err;
		}

		type = res._ground_fix_resp_fulfilledWith._methods_choice;
		if (type == _methods_MCELL_tstr) {
			result->type = LOCATION_TYPE_MULTI_CELL;
		} else if (type == _methods_SCELL_tstr) {
			result->type = LOCATION_TYPE_SINGLE_CELL;
		} else if (type == _methods_WIFI_tstr) {
			result->type = LOCATION_TYPE_WIFI;
		} else {
			result->type = LOCATION_TYPE__INVALID;
			LOG_WRN("Unhandled location type: %d", type);
		}

		result->lat = res._ground_fix_resp_lat;
		result->lon = res._ground_fix_resp_lon;

		if (res._ground_fix_resp_uncertainty_choice == _ground_fix_resp_uncertainty_int) {
			result->unc = res._ground_fix_resp_uncertainty_int;
		} else {
			result->unc = (uint32_t)lround(res._ground_fix_resp_uncertainty_float);
		}
	} else {
		err = nrf_cloud_location_response_decode((const char *const)buf, result);
	}
	return err;
}

#if defined(CONFIG_NRF_CLOUD_AGPS)
#define ALL_TYPES (NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST | \
		   NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST | \
		   NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST | \
		   NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST | \
		   NRF_MODEM_GNSS_AGPS_POSITION_REQUEST | \
		   NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST)

static bool agps_all_types_set(struct nrf_modem_gnss_agps_data_frame const *const req)
{
	return ((req->sv_mask_ephe != 0) && (req->sv_mask_alm != 0) &&
		((req->data_flags & (ALL_TYPES)) == ALL_TYPES));
}

static int format_agps_custom_types_array(struct nrf_modem_gnss_agps_data_frame const *const req,
	int *array, int array_len)
{
	__ASSERT_NO_MSG(req != NULL);
	__ASSERT_NO_MSG(array != NULL);
	__ASSERT_NO_MSG(array_len >= (NRF_CLOUD_AGPS__LAST + 1));

	int pos = 0;

	if (req->data_flags & NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_UTC_PARAMETERS;
	}
	if (req->sv_mask_ephe) {
		array[pos++] = NRF_CLOUD_AGPS_EPHEMERIDES;
	}
	if (req->sv_mask_alm) {
		array[pos++] = NRF_CLOUD_AGPS_ALMANAC;
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_KLOBUCHAR_CORRECTION;
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_NEQUICK_CORRECTION;
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_GPS_TOWS;
		array[pos++] = NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK;
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_LOCATION;
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST) {
		array[pos++] = NRF_CLOUD_AGPS_INTEGRITY;
	}

	return pos;
}

/* Macro to print the comma separated list of custom types */
#define AGPS_TYPE_PRINT(buf, type)		\
	if (pos != 0) {				\
		buf[pos++] = ',';		\
	}					\
	buf[pos++] = (char)('0' + type)

static int format_agps_custom_types_str(struct nrf_modem_gnss_agps_data_frame const *const req,
	char *const types_buf)
{
	__ASSERT_NO_MSG(req != NULL);
	__ASSERT_NO_MSG(types_buf != NULL);

	int pos = 0;

	if (req->data_flags & NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_UTC_PARAMETERS);
	}
	if (req->sv_mask_ephe) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_EPHEMERIDES);
	}
	if (req->sv_mask_alm) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_ALMANAC);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_KLOBUCHAR_CORRECTION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_NEQUICK_CORRECTION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_GPS_TOWS);
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_LOCATION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_INTEGRITY);
	}

	types_buf[pos] = '\0';

	return pos ? 0 : -EBADF;
}

int coap_codec_encode_agps(struct nrf_cloud_rest_agps_request const *const request,
			   uint8_t *buf, size_t *len, bool *query_string,
			   enum coap_content_format fmt)
{
	int ret;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
#if 1
		struct agps_req input;
		struct agps_req_types_ *t = &input._agps_req_types;
		size_t out_len;

		memset(&input, 0, sizeof(struct agps_req));
		input._agps_req_eci = request->net_info->current_cell.id;
		input._agps_req_mcc = request->net_info->current_cell.mcc;
		input._agps_req_mnc = request->net_info->current_cell.mnc;
		input._agps_req_tac = request->net_info->current_cell.tac;
		if (agps_all_types_set(request->agps_req) &&
		    (request->type == NRF_CLOUD_REST_AGPS_REQ_CUSTOM)) {
			input._agps_req_requestType._agps_req_requestType._type_choice =
				_type__rtAssistance;
			input._agps_req_requestType_present = true;
		} else if (request->type == NRF_CLOUD_REST_AGPS_REQ_CUSTOM) {
			t->_agps_req_types_int_count = format_agps_custom_types_array(
							request->agps_req,
							t->_agps_req_types_int,
							ARRAY_SIZE(t->_agps_req_types_int));
			input._agps_req_types_present = true;
			input._agps_req_requestType._agps_req_requestType._type_choice =
				_type__custom;
			input._agps_req_requestType_present = true;
		} else {
			input._agps_req_requestType._agps_req_requestType._type_choice =
				_type__rtAssistance;
			input._agps_req_requestType_present = true;
		}
		if (request->filtered) {
			input._agps_req_filtered_present = true;
			input._agps_req_filtered._agps_req_filtered = true;
			if (request->mask_angle != 5) {
				input._agps_req_mask_present = true;
				input._agps_req_mask._agps_req_mask = request->mask_angle;
			}
		}
		if (request->net_info->current_cell.rsrp != NRF_CLOUD_LOCATION_CELL_OMIT_RSRP) {
			input._agps_req_rsrp._agps_req_rsrp = request->net_info->current_cell.rsrp;
			input._agps_req_rsrp_present = true;
		}
		ret = cbor_encode_agps_req(buf, *len, &input, &out_len);
		if (ret) {
			LOG_ERR("Error %d encoding A-GPS req", ret);
			ret = -EINVAL;
			*len = 0;
		} else {
			*len = out_len;
		}
#else
		struct agps input;
		struct agps_type_ *t = input.__agps_type;
		struct lte_lc_cell *c = &request->net_info->current_cell;
		size_t out_len;

		memset(&input, 0, sizeof(struct agps));

		t->_agps_type_choice = _agps_type__eci;
		t->_agps_type__eci._eci_val = c->id;
		t++;

		t->_agps_type_choice = _agps_type__mcc;
		t->_agps_type__mcc._mcc_val = c->mcc;
		t++;

		t->_agps_type_choice = _agps_type__mnc;
		t->_agps_type__mnc._mnc_val = c->mnc;
		t++;

#endif
	} else {
		char *url = (char *)buf;
		char custom_types[NRF_CLOUD_AGPS__LAST * 2];
		size_t pos = 0;
		size_t remain = *len;

		*query_string = true;
		*url = '\0';
		if (request->filtered) {
			ret = snprintk(&url[pos], remain, AGPS_FILTERED);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: filtered");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
			ret = snprintk(&url[pos], remain, AGPS_ELEVATION_MASK, request->mask_angle);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: mask angle");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (agps_all_types_set(request->agps_req) &&
		    (request->type == NRF_CLOUD_REST_AGPS_REQ_CUSTOM)) {
		} else if (request->type == NRF_CLOUD_REST_AGPS_REQ_CUSTOM) {
			ret = format_agps_custom_types_str(request->agps_req, custom_types);
			ret = snprintk(&url[pos], remain, AGPS_CUSTOM_TYPE, custom_types);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: custom types");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (request->net_info) {
			ret = snprintk(&url[pos], remain, AGPS_NET_INFO,
				       request->net_info->current_cell.mcc,
				       request->net_info->current_cell.mnc,
				       request->net_info->current_cell.tac,
				       request->net_info->current_cell.id);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: network info");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (buf[0] == '&') {
			char *s = &buf[1];
			char *d = buf;

			while (*s) {
				*(d++) = *(s++);
			}
			*d = '\0';
		}
		*len = pos;
		LOG_INF("A-GPS query: %s", (const char *)buf);
		return 0;

clean_up:
		buf[0] = 0;
		*len = 0;
	}
	return ret;
}

int coap_codec_decode_agps_resp(struct nrf_cloud_rest_agps_result *result,
				const uint8_t *buf, size_t len, enum coap_content_format fmt)
{
	if (fmt == COAP_CONTENT_FORMAT_APP_JSON) {
		LOG_ERR("Error on A-GPS: %*s", len, (const char *)buf);
		return -EFAULT;
	}
	if (fmt != COAP_CONTENT_FORMAT_APP_OCTET_STREAM) {
		LOG_ERR("Invalid format for A-GPS data: %d", fmt);
		return -ENOTSUP;
	}
	if (!result) {
		return -EINVAL;
	}

	if (result->buf_sz < len) {
		LOG_WRN("A-GPS buffer is too small; expected: %zd, got:%zd; truncated",
			len, result->buf_sz);
	}
	result->agps_sz = MIN(result->buf_sz, len);
	memcpy(result->buf, buf, result->agps_sz);
	return 0;
}
#endif /* CONFIG_NRF_CLOUD_AGPS */

#if defined(CONFIG_NRF_CLOUD_PGPS)
int coap_codec_encode_pgps(struct nrf_cloud_rest_pgps_request const *const request,
			   uint8_t *buf, size_t *len, bool *query_string,
			   enum coap_content_format fmt)
{
	int ret;
	__ASSERT_NO_MSG(request != NULL);
	__ASSERT_NO_MSG(request->pgps_req != NULL);

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		struct pgps_req input;
		size_t out_len;

		memset(&input, 0, sizeof(input));

		input._pgps_req_predictionCount = request->pgps_req->prediction_count;
		input._pgps_req_predictionIntervalMinutes = request->pgps_req->prediction_period_min;
		input._pgps_req_startGPSDay = request->pgps_req->gps_day;
		input._pgps_req_startGPSTimeOfDaySeconds = request->pgps_req->gps_time_of_day;

		ret = cbor_encode_pgps_req(buf, *len, &input, &out_len);
		if (ret) {
			LOG_ERR("Error %d encoding P-GPS req", ret);
			ret = -EINVAL;
			*len = 0;
		} else {
			*len = out_len;
		}
	} else {
		char *query = (char *)buf;
		size_t pos = 0;
		size_t remain = *len;

		if (request->pgps_req->prediction_count !=
		    NRF_CLOUD_PGPS_REQ_NO_COUNT) {
			ret = snprintk(&query[pos], remain, PGPS_REQ_PREDICT_CNT,
				request->pgps_req->prediction_count);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: prediction count");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (request->pgps_req->prediction_period_min !=
		    NRF_CLOUD_PGPS_REQ_NO_INTERVAL) {
			ret = snprintk(&query[pos], remain, PGPS_REQ_PREDICT_INT_MIN,
				       request->pgps_req->prediction_period_min);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: prediction interval");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (request->pgps_req->gps_day !=
		    NRF_CLOUD_PGPS_REQ_NO_GPS_DAY) {
			ret = snprintk(&query[pos], remain, PGPS_REQ_START_GPS_DAY,
				       request->pgps_req->gps_day);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: GPS day");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (request->pgps_req->gps_time_of_day !=
		    NRF_CLOUD_PGPS_REQ_NO_GPS_TOD) {
			ret = snprintk(&query[pos], remain, PGPS_REQ_START_GPS_TOD_S,
				       request->pgps_req->gps_time_of_day);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: GPS time");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}
		*len = pos;
		LOG_INF("P-GPS query: %s", (const char *)buf);
		ret = 0;

clean_up:
		buf[0] = 0;
		*len = 0;
	}

	return ret;
}

int coap_codec_decode_pgps_resp(struct nrf_cloud_pgps_result *result,
				const uint8_t *buf, size_t len, enum coap_content_format fmt)
{
	if (fmt != COAP_CONTENT_FORMAT_APP_JSON) {
		LOG_ERR("Invalid format for P-GPS: %d", fmt);
		return -ENOTSUP;
	}
	return nrf_cloud_pgps_response_decode((const char *)buf, result);
}
#endif /* CONFIG_NRF_CLOUD_PGPS */

int coap_codec_decode_fota_resp(struct nrf_cloud_fota_job_info * job,
				const uint8_t *buf, size_t len, enum coap_content_format fmt)
{
	if (!job) {
		return -EINVAL;
	}
	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		return -ENOTSUP;
	} else {
		return nrf_cloud_rest_fota_execution_decode(buf, job);
	}
}

