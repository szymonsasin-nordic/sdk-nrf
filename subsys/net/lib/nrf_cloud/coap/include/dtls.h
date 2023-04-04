/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DTLS_H_
#define DTLS_H_

int provision_psk(void);
int provision_ca(void);
int dtls_init(int sock);
int dtls_print_connection_id(int sock, bool verbose);
int get_device_id(char *buf, size_t len);

#endif /* DTLS_H_ */
