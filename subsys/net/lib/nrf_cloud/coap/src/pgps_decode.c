/*
 * Generated using zcbor version 0.5.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "pgps_decode.h"

#if DEFAULT_MAX_QTY != 10
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_pgps_resp(zcbor_state_t *state, const struct pgps_resp *input);


static bool encode_pgps_resp(
		zcbor_state_t *state, const struct pgps_resp *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_map_start_encode(state, 2) && (((((zcbor_uint32_put(state, (1))))
	&& (zcbor_tstr_encode(state, (&(*input)._pgps_resp_host))))
	&& (((zcbor_uint32_put(state, (2))))
	&& (zcbor_tstr_encode(state, (&(*input)._pgps_resp_path))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 2))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_pgps_resp(
		uint8_t *payload, size_t payload_len,
		const struct pgps_resp *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[3];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_pgps_resp(states, input);

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
