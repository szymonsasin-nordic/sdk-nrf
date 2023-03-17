/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "agps_encode.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_repeated_agps_req_customTypes(zcbor_state_t *state, const struct agps_req_customTypes_ *input);
static bool encode_repeated_agps_req_filtered(zcbor_state_t *state, const struct agps_req_filtered *input);
static bool encode_repeated_agps_req_mask(zcbor_state_t *state, const struct agps_req_mask *input);
static bool encode_repeated_agps_req_rsrp(zcbor_state_t *state, const struct agps_req_rsrp *input);
static bool encode_agps_req(zcbor_state_t *state, const struct agps_req *input);


static bool encode_repeated_agps_req_customTypes(
		zcbor_state_t *state, const struct agps_req_customTypes_ *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"customTypes", tmp_str.len = sizeof("customTypes") - 1, &tmp_str)))))
	&& (zcbor_list_start_encode(state, 10) && ((zcbor_multi_encode_minmax(1, 10, &(*input)._agps_req_customTypes_int_count, (zcbor_encoder_t *)zcbor_int32_encode, state, (&(*input)._agps_req_customTypes_int), sizeof(int32_t))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 10))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_agps_req_filtered(
		zcbor_state_t *state, const struct agps_req_filtered *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"filtered", tmp_str.len = sizeof("filtered") - 1, &tmp_str)))))
	&& (zcbor_bool_encode(state, (&(*input)._agps_req_filtered)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_agps_req_mask(
		zcbor_state_t *state, const struct agps_req_mask *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"mask", tmp_str.len = sizeof("mask") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._agps_req_mask)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_agps_req_rsrp(
		zcbor_state_t *state, const struct agps_req_rsrp *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rsrp", tmp_str.len = sizeof("rsrp") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input)._agps_req_rsrp)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_agps_req(
		zcbor_state_t *state, const struct agps_req *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 9) && ((zcbor_present_encode(&((*input)._agps_req_customTypes_present), (zcbor_encoder_t *)encode_repeated_agps_req_customTypes, state, (&(*input)._agps_req_customTypes))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"eci", tmp_str.len = sizeof("eci") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._agps_req_eci))))
	&& zcbor_present_encode(&((*input)._agps_req_filtered_present), (zcbor_encoder_t *)encode_repeated_agps_req_filtered, state, (&(*input)._agps_req_filtered))
	&& zcbor_present_encode(&((*input)._agps_req_mask_present), (zcbor_encoder_t *)encode_repeated_agps_req_mask, state, (&(*input)._agps_req_mask))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"mcc", tmp_str.len = sizeof("mcc") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._agps_req_mcc))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"mnc", tmp_str.len = sizeof("mnc") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._agps_req_mnc))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"requestType", tmp_str.len = sizeof("requestType") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._agps_req_requestType))))
	&& zcbor_present_encode(&((*input)._agps_req_rsrp_present), (zcbor_encoder_t *)encode_repeated_agps_req_rsrp, state, (&(*input)._agps_req_rsrp))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"tac", tmp_str.len = sizeof("tac") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._agps_req_tac))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 9))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_agps_req(
		uint8_t *payload, size_t payload_len,
		const struct agps_req *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[4];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_agps_req(states, input);

	if (ret && (payload_len_out != NULL)) {
		*payload_len_out = MIN(payload_len,
				(size_t)states[0].payload - (size_t)payload);
	}

	if (!ret) {
		int err = zcbor_pop_error(states);

		zcbor_print("Return error: %d\r\n", err);
		return (err == ZCBOR_SUCCESS) ? ZCBOR_ERR_UNKNOWN : err;
	}
	return ZCBOR_SUCCESS;
}
