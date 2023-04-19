/*
 * Generated using zcbor version 0.5.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 10
 */

#ifndef GROUND_FIX_DECODE_TYPES_H__
#define GROUND_FIX_DECODE_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_decode.h"

/** Which value for --default-max-qty this file was created with.
 *
 *  The define is used in the other generated file to do a build-time
 *  compatibility check.
 *
 *  See `zcbor --help` for more information about --default-max-qty
 */
#define DEFAULT_MAX_QTY 10

struct methods_ {
	enum {
		_methods__MCELL = 5,
		_methods__SCELL = 6,
		_methods__WIFI = 7,
	} _methods_choice;
};

struct ground_fix_resp {
	struct methods_ _ground_fix_resp_fulfilledWith;
	double _ground_fix_resp_lat;
	double _ground_fix_resp_lon;
	union {
		int32_t _ground_fix_resp_uncertainty_int;
		double _ground_fix_resp_uncertainty_float;
	};
	enum {
		_ground_fix_resp_uncertainty_int,
		_ground_fix_resp_uncertainty_float,
	} _ground_fix_resp_uncertainty_choice;
};


#endif /* GROUND_FIX_DECODE_TYPES_H__ */
