/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef MSG_ENCODE_H__
#define MSG_ENCODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "msg_encode_types.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif


int cbor_encode_message_out(
		uint8_t *payload, size_t payload_len,
		const struct message_out *input,
		size_t *payload_len_out);


#endif /* MSG_ENCODE_H__ */
