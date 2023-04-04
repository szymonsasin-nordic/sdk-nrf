/*
 * Generated using zcbor version 0.5.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef PGPS_DECODE_H__
#define PGPS_DECODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "pgps_decode_types.h"

#if DEFAULT_MAX_QTY != 10
#error "The type file was generated with a different default_max_qty than this file"
#endif


int cbor_encode_pgps_resp(
		uint8_t *payload, size_t payload_len,
		const struct pgps_resp *input,
		size_t *payload_len_out);


#endif /* PGPS_DECODE_H__ */
