/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef COAP_CLIENT_H_
#define COAP_CLIENT_H_

enum nrf_cloud_coap_response
{
	NRF_CLOUD_COAP_NONE,
	NRF_CLOUD_COAP_LOCATION,
	NRF_CLOUD_COAP_AGPS,
	NRF_CLOUD_COAP_PGPS,
	NRF_CLOUD_COAP_FOTA_JOB
};

#define APP_COAP_JWT_ACK_WAIT_MS 120000
#define APP_COAP_SEND_INTERVAL_MS 10000
#define APP_COAP_RECEIVE_INTERVAL_MS 5000
#define APP_COAP_CLOSE_THRESHOLD_MS 4000
#define APP_COAP_CONNECTION_CHECK_MS 30000
#define APP_COAP_INTERVAL_LIMIT 60

typedef int (*coap_callback)(const void *buf, size_t len, enum coap_content_format format,
			     void *user);

int client_get_sock(void);

int client_init(void);

int client_provision(bool force);

int client_wait_data(int timeout);

int client_wait_ack(int wait_ms);

int client_receive(int timeout);

/**@brief Handles responses from the remote CoAP server. */
int client_handle_response(uint8_t *buf, int received);

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt_out,
		    enum coap_content_format fmt_in,
		    coap_callback cb, void *user);

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt, bool reliable,
		    coap_callback cb, void *user);

/**@brief Send CoAP PUT request. */
int client_put_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt,
		    coap_callback cb, void *user);

/**@brief Send CoAP DELETE request. */
int client_delete_send(const char *resource, const char *query,
		       uint8_t *buf, size_t len,
		       enum coap_content_format fmt,
		       coap_callback cb, void *user);

/**@brief Send CoAP FETCH request. */
int client_fetch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt_out,
		      enum coap_content_format fmt_in,
		      coap_callback cb, void *user);

/**@brief Send CoAP PATCH request. */
int client_patch_send(const char *resource, const char *query,
  		    uint8_t *buf, size_t len,
  		    enum coap_content_format fmt,
		    coap_callback cb, void *user);

int client_close(void);

#endif /* COAP_CLIENT_H_ */
