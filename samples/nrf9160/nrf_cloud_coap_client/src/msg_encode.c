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
#include "msg_encode.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_repeated_message_out_topic(zcbor_state_t *state, const struct message_out_topic *input);
static bool encode_repeated_data_ob_ts(zcbor_state_t *state, const struct data_ob_ts *input);
static bool encode_data_ob(zcbor_state_t *state, const struct data_ob *input);
static bool encode_message_out(zcbor_state_t *state, const struct message_out *input);


static bool encode_repeated_message_out_topic(
		zcbor_state_t *state, const struct message_out_topic *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"topic", tmp_str.len = sizeof("topic") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._message_out_topic)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_data_ob_ts(
		zcbor_state_t *state, const struct data_ob_ts *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"ts", tmp_str.len = sizeof("ts") - 1, &tmp_str)))))
	&& ((((*input)._data_ob_ts <= 18446744073709551615ULL)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint64_encode(state, (&(*input)._data_ob_ts)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_data_ob(
		zcbor_state_t *state, const struct data_ob *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 4) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"appId", tmp_str.len = sizeof("appId") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._data_ob_appId))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"messageType", tmp_str.len = sizeof("messageType") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._data_ob_messageType))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"data", tmp_str.len = sizeof("data") - 1, &tmp_str)))))
	&& (((*input)._data_ob_data_choice == _data_ob_data_tstr) ? ((zcbor_tstr_encode(state, (&(*input)._data_ob_data_tstr))))
	: (((*input)._data_ob_data_choice == _data_ob_data_float) ? ((zcbor_float64_encode(state, (&(*input)._data_ob_data_float))))
	: false)))
	&& zcbor_present_encode(&((*input)._data_ob_ts_present), (zcbor_encoder_t *)encode_repeated_data_ob_ts, state, (&(*input)._data_ob_ts))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 4))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_message_out(
		zcbor_state_t *state, const struct message_out *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 2) && ((zcbor_present_encode(&((*input)._message_out_topic_present), (zcbor_encoder_t *)encode_repeated_message_out_topic, state, (&(*input)._message_out_topic))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"message", tmp_str.len = sizeof("message") - 1, &tmp_str)))))
	&& (encode_data_ob(state, (&(*input)._message_out_message))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 2))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_message_out(
		uint8_t *payload, size_t payload_len,
		const struct message_out *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[5];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_message_out(states, input);

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
