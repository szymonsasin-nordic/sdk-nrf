/*
 * Generated using zcbor version 0.5.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef GROUND_FIX_DECODE_H__
#define GROUND_FIX_DECODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_decode.h"
#include "ground_fix_decode_types.h"

#if DEFAULT_MAX_QTY != 10
#error "The type file was generated with a different default_max_qty than this file"
#endif


int cbor_decode_ground_fix_resp(
		const uint8_t *payload, size_t payload_len,
		struct ground_fix_resp *result,
		size_t *payload_len_out);


#endif /* GROUND_FIX_DECODE_H__ */
