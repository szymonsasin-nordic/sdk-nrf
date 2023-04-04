/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef APP_JWT_H_
#define APP_JWT_H_

#define JWT_DURATION_S (60 * 5)

int app_jwt_init(void);
int app_jwt_generate(uint32_t time_valid_s, char *const jwt_buf, size_t jwt_buf_sz);

#endif /* APP_JWT_H_ */
