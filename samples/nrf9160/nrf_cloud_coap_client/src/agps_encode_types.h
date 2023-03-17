/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef AGPS_ENCODE_TYPES_H__
#define AGPS_ENCODE_TYPES_H__

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

struct agps_req_customTypes_ {
	int32_t _agps_req_customTypes_int[10];
	uint_fast32_t _agps_req_customTypes_int_count;
};

struct agps_req_filtered {
	bool _agps_req_filtered;
};

struct agps_req_mask {
	uint32_t _agps_req_mask;
};

struct agps_req_rsrp {
	int32_t _agps_req_rsrp;
};

struct agps_req {
	struct agps_req_customTypes_ _agps_req_customTypes;
	uint_fast32_t _agps_req_customTypes_present;
	uint32_t _agps_req_eci;
	struct agps_req_filtered _agps_req_filtered;
	uint_fast32_t _agps_req_filtered_present;
	struct agps_req_mask _agps_req_mask;
	uint_fast32_t _agps_req_mask_present;
	uint32_t _agps_req_mcc;
	uint32_t _agps_req_mnc;
	struct zcbor_string _agps_req_requestType;
	struct agps_req_rsrp _agps_req_rsrp;
	uint_fast32_t _agps_req_rsrp_present;
	uint32_t _agps_req_tac;
};


#endif /* AGPS_ENCODE_TYPES_H__ */
