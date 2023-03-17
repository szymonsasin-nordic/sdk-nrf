/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef MSG_ENCODE_TYPES_H__
#define MSG_ENCODE_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"

/** Which value for --default-max-qty this file was created with.
 *
 *  The define is used in the other generated file to do a build-time
 *  compatibility check.
 *
 *  See `zcbor --help` for more information about --default-max-qty
 */
#define DEFAULT_MAX_QTY 3

struct message_out_topic {
	struct zcbor_string _message_out_topic;
};

struct data_ob_ts {
	uint64_t _data_ob_ts;
};

struct data_ob {
	struct zcbor_string _data_ob_appId;
	struct zcbor_string _data_ob_messageType;
	union {
		struct zcbor_string _data_ob_data_tstr;
		double _data_ob_data_float;
	};
	enum {
		_data_ob_data_tstr,
		_data_ob_data_float,
	} _data_ob_data_choice;
	struct data_ob_ts _data_ob_ts;
	uint_fast32_t _data_ob_ts_present;
};

struct message_out {
	struct message_out_topic _message_out_topic;
	uint_fast32_t _message_out_topic_present;
	struct data_ob _message_out_message;
};


#endif /* MSG_ENCODE_TYPES_H__ */
