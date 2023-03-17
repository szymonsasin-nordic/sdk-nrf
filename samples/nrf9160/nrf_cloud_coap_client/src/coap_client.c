/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <zephyr/random/rand32.h>
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <cJSON.h>
#include <version.h>
#include "dtls.h"
#include "app_jwt.h"
#include "coap_codec.h"
#include "coap_client.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

/* Uncomment to enable sending cell_pos parameters with GET as payload */
//#define CELL_POS_PAYLOAD

#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

static struct sockaddr_storage server;

static int sock;
static struct pollfd fds;

static struct connection_info
{
	uint8_t s4_addr[4];
	uint8_t d4_addr[4];
} connection_info;

static uint8_t coap_buf[APP_COAP_MAX_MSG_LEN];

static sys_dlist_t con_messages;
static int num_con_messages;

struct nrf_cloud_coap_message {
	sys_dnode_t node;
	uint16_t message_id;
	uint16_t token_len;
	uint8_t token[8];
	coap_callback cb;
	void *user;
};

static const char *coap_methods[] = {
	"NONE",
	"GET",
	"POST",
	"PUT",
	"DELETE",
	"FETCH",
	"PATCH",
	"IPATCH"
};

static const char *coap_types[] = {
	"CON", "NON", "ACK", "RST", NULL
};

struct content_type {
	int type;
	bool text;
	const char *name;
};

static const struct content_type coap_content_types[] = {
	{COAP_CONTENT_FORMAT_TEXT_PLAIN, true, "text plain"},
	{COAP_CONTENT_FORMAT_APP_LINK_FORMAT, true, "link format"},
	{COAP_CONTENT_FORMAT_APP_XML, true, "XML"},
	{COAP_CONTENT_FORMAT_APP_OCTET_STREAM, false, "octet stream"},
	{COAP_CONTENT_FORMAT_APP_EXI, false, "EXI"},
	{COAP_CONTENT_FORMAT_APP_JSON, true, "JSON"},
	{COAP_CONTENT_FORMAT_APP_JSON_PATCH_JSON, true, "JSON patch"},
	{COAP_CONTENT_FORMAT_APP_MERGE_PATCH_JSON, true, "JSON merge"},
	{COAP_CONTENT_FORMAT_APP_CBOR, false, "CBOR"},
	{0, false, NULL}
};

static char jwt[700];
static uint16_t next_token;

static K_SEM_DEFINE(ready_sem, 0, 1);
static K_SEM_DEFINE(con_ack_sem, 0, 1);

/**@brief Resolves the configured hostname. */
static int server_resolve(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM
	};
	char ipv4_addr[NET_IPV4_ADDR_LEN];

	LOG_DBG("Looking up server %s", CONFIG_COAP_SERVER_HOSTNAME);
	err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
	if (err != 0) {
		LOG_ERR("ERROR: getaddrinfo for %s failed: %d",
			CONFIG_COAP_SERVER_HOSTNAME, err);
		return -EIO;
	}

	if (result == NULL) {
		LOG_ERR("ERROR: Address not found");
		return -ENOENT;
	}

	/* IPv4 Address. */
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&server);

	server4->sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_COAP_SERVER_PORT);

	connection_info.s4_addr[0] = server4->sin_addr.s4_addr[0];
	connection_info.s4_addr[1] = server4->sin_addr.s4_addr[1];
	connection_info.s4_addr[2] = server4->sin_addr.s4_addr[2];
	connection_info.s4_addr[3] = server4->sin_addr.s4_addr[3];

	inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr,
		  sizeof(ipv4_addr));
	LOG_INF("Server %s IP address: %s, port: %u",
		CONFIG_COAP_SERVER_HOSTNAME, ipv4_addr, CONFIG_COAP_SERVER_PORT);

	/* Free the address. */
	freeaddrinfo(result);

	return 0;
}

int client_get_sock(void)
{
	return sock;
}

/**@brief Initialize the CoAP client */
int client_init(void)
{
	int err;

	sys_dlist_init(&con_messages);

	err = server_resolve();
	if (err) {
		LOG_ERR("Failed to resolve server name: %d", err);
		return err;
	}

	LOG_DBG("Creating socket");
#if !defined(CONFIG_COAP_DTLS)
	LOG_DBG("IPPROTO_UDP");
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	LOG_DBG("IPPROTO_DTLS_1_2");
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
#else
	LOG_DBG("SPLIT STACK IPPROTO_DTLS_1_2");
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NATIVE_TLS, IPPROTO_DTLS_1_2);
#endif
#endif
	LOG_DBG("sock = %d", sock);
	if (sock < 0) {
		LOG_ERR("Failed to create CoAP socket: %d.", -errno);
		return -errno;
	}

#if defined(CONFIG_COAP_DTLS)
	err = dtls_init(sock);
	if (err < 0) {
		LOG_ERR("Failed to initialize the DTLS client: %d", err);
		return err;
	}
#endif
	err = connect(sock, (struct sockaddr *)&server,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Connect failed : %d", -errno);
		return -errno;
	} else {
		LOG_INF("Connect succeeded.");
	}

	/* Initialize FDS, for poll. */
	fds.fd = sock;
	fds.events = POLLIN;

	/* Randomize token. */
	next_token = sys_rand32_get();

#if !defined(CONFIG_COAP_DTLS_PSK)
	LOG_DBG("Generate JWT");
	err = app_jwt_generate(NRF_CLOUD_JWT_VALID_TIME_S_MAX, jwt, sizeof(jwt));
	if (err) {
		return err;
	}

	char ver_string[120] = {0};
	char mfw_string[60];

	err = modem_info_get_fw_version(mfw_string, sizeof(mfw_string));
	if (!err) {
		snprintf(ver_string, sizeof(ver_string) - 1, "mver=%s&cver=%s",
			 mfw_string, STRINGIFY(BUILD_VERSION));
	} else {
		LOG_ERR("Unable to obtain the modem firmware version: %d", err);
	}
	LOG_DBG("Send JWT");
	err = client_post_send("poc/auth/jwt", err ? NULL : ver_string,
			       (uint8_t *)jwt, strlen(jwt),
			       COAP_CONTENT_FORMAT_TEXT_PLAIN, NULL, NULL);

	k_sem_give(&ready_sem);
#endif
	return err;
}

int client_provision(bool force)
{
	int err = 0;

	if (force || IS_ENABLED(CONFIG_NET_SOCKETS_ENABLE_DTLS)) {
#if defined(CONFIG_COAP_DTLS_PSK)
		err = provision_psk();
#else
		err = provision_ca();
#endif
	}
	return err;
}

/* Returns 0 if data is available.
 * Returns -EAGAIN if timeout occured and there is no data.
 * Returns other, negative error code in case of poll error.
 */
int client_wait_data(int timeout)
{
	int ret = poll(&fds, 1, timeout);

	if (ret < 0) {
		LOG_ERR("poll error: %d", -errno);
		return -errno;
	}

	if (ret == 0) {
		/* Timeout. */
		return -EAGAIN;
	}

	if ((fds.revents & POLLERR) == POLLERR) {
		LOG_ERR("wait: POLLERR");
		return -EIO;
	}

	if ((fds.revents & POLLNVAL) == POLLNVAL) {
		LOG_ERR("wait: POLLNVAL");
		return -EBADF;
	}

	if ((fds.revents & POLLIN) != POLLIN) {
		return -EAGAIN;
	}

	return 0;
}

int client_wait_ack(int wait_ms)
{
	int err;

	LOG_DBG("Wait for ACK");
	err = k_sem_take(&con_ack_sem, K_MSEC(wait_ms));
	if (err) {
		LOG_ERR("Error waiting for JWT ACK: %d", err);
	}
	return err;
}

int client_receive(int timeout)
{
	int err;
	int received;

	LOG_DBG("Waiting for response");
	do {
		err = client_wait_data(timeout);
		if (err < 0) {
			if (err != -EAGAIN) {
				LOG_ERR("Poll error: %d", err);
				return err;
			}
		}
	} while (err);

	LOG_DBG("Calling recv()");
	received = recv(sock, coap_buf, sizeof(coap_buf), MSG_DONTWAIT);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			LOG_WRN("socket EAGAIN");
			return 0;
		} else {
			LOG_ERR("Socket error: %d", -errno);
			return -errno;
		}
	}

	if (received == 0) {
		LOG_WRN("Empty datagram");
		return 0;
	}

	LOG_DBG("Calling client_handle_get_response()");
	err = client_handle_response(coap_buf, received);
	if (err < 0) {
		LOG_ERR("Invalid response: %d", err);
	}

	return err;
}

/**@brief Send CoAP ACK or RST response. */
static int client_response(struct coap_packet *req,
			   const char *resource, uint16_t mid,
			   uint16_t token_len, uint8_t *token, bool ack)
{
	int err;
	struct coap_packet response;
	enum coap_msgtype msg_type = ack ? COAP_TYPE_ACK : COAP_TYPE_RESET;

	err = coap_packet_init(&response, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       token_len, token, 0, mid);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP response, %d", err);
		return err;
	}

	if (resource && strlen(resource)) {
		err = coap_packet_append_option(&response, COAP_OPTION_URI_PATH,
						(uint8_t *)resource,
						strlen(resource));
		if (err < 0) {
			LOG_ERR("Failed to encode CoAP option, %d", err);
			return err;
		}
	}

	err = send(sock, response.data, response.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP response, %d", -errno);
		return -errno;
	}

	LOG_INF("CoAP %s response sent: token 0x%04x", coap_types[msg_type], next_token);

	return 0;
}

static int remove_msg_from_list(struct nrf_cloud_coap_message *msg)
{
	if (msg != NULL) {
		sys_dlist_remove(&msg->node);
		k_free(msg);
		if (num_con_messages) {
			num_con_messages--;
		}
		LOG_DBG("messages left: %d", num_con_messages);
	}
	return 0;
}

static int find_response(uint8_t *token, uint16_t token_len, uint16_t message_id,
			 uint8_t code, uint8_t type, uint16_t payload_len,
			 coap_callback *cb, void **user)
{
	struct nrf_cloud_coap_message *msg = NULL;
	int err = -ENOMSG;

	SYS_DLIST_FOR_EACH_CONTAINER(&con_messages, msg, node) {
		//LOG_DBG("  mid:0x%04x, Token:0x%02x%02x ?",
		//	msg->message_id, msg->token[1], msg->token[0]);
		if ((type == COAP_TYPE_CON) || (type == COAP_TYPE_NON_CON)) {
			/* match token only */
			if ((token_len == msg->token_len) &&
			    (memcmp(msg->token, token, token_len) == 0)) {
				LOG_DBG("Matched token");
				err = 0;
				break;
			} else {
				//LOG_DBG("  token not found yet");
			}
		} else { /* ACK or RESET */
			/* EMPTY responses: match MID only */
			if (code == 0) { 
				if (msg->message_id == message_id) {
					LOG_DBG("Matched empty %s.",
						(type == COAP_TYPE_ACK) ? "ACK" : "RESET");
					err = 0;
					if (type == COAP_TYPE_ACK) {
						LOG_DBG("Empty ACK");
						k_sem_give(&con_ack_sem);
					}
					break;
				} else {
					//LOG_DBG("  MID not found yet");
				}
			} else {
				if ((msg->message_id == message_id) &&
				    (token_len == msg->token_len) &&
				    (memcmp(msg->token, token, token_len) == 0)) {
					LOG_DBG("Found MID and token");
					err = 0;
					if (type == COAP_TYPE_ACK) {
						k_sem_give(&con_ack_sem);
						LOG_DBG("ACK with code: %u", payload_len);
						if (payload_len) {
							/* piggy-backed response, so remove
							 * this one and then look for and
							 * remove the second (at end of handler)
							 */
							LOG_DBG("Piggy-backed response");
							err = EAGAIN; /* positive value */
							break;
						}
					}
					break;
				} else {
					//LOG_DBG("  MID and token not found yet");
				}
			}
		}
	}
	if (err == 0) {
		*cb = msg->cb;
		*user = msg->user;
	}
	if (err >= 0) {
		LOG_DBG("Removing");
		remove_msg_from_list(msg);
	}
	return err;
}

/**@brief Handles responses from the remote CoAP server. */
int client_handle_response(uint8_t *buf, int received)
{
	int err;
	struct coap_packet reply;
	struct coap_option options[16] = {};
	int count;
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t token[8] = {0};
	uint16_t token_len;
	char uri_path[64];
	enum coap_content_format format = COAP_CONTENT_FORMAT_TEXT_PLAIN;
	uint16_t message_id;
	uint8_t code;
	uint8_t type;
	coap_callback cb;
	void *user;

	err = coap_packet_parse(&reply, buf, received, NULL, 0);
	if (err < 0) {
		LOG_ERR("Malformed response received: %d", err);
		goto done;
	}

	token_len = coap_header_get_token(&reply, token);
	message_id = coap_header_get_id(&reply);
	code = coap_header_get_code(&reply);
	type = coap_header_get_type(&reply);
	
	if (type > COAP_TYPE_RESET) {
		LOG_ERR("Illegal CoAP type: %u", type);
		err = -EINVAL;
		goto done;
	}

	count = ARRAY_SIZE(options) - 1;
	count = coap_find_options(&reply, COAP_OPTION_URI_PATH,
				   options, count);
	if (count > 1) {
		LOG_ERR("Unexpected number of URI path options: %d", count);
		err = -EINVAL;
		goto done;
	} else if (count == 1) {
		memcpy(uri_path, options[0].value, options[0].len);
		uri_path[options[0].len] = '\0';
	} else {
		uri_path[0] = '\0';
	}

	payload = coap_packet_get_payload(&reply, &payload_len);

	LOG_INF("Got response uri:%s, code:0x%02x (%d.%02d), type:%u %s, "
		"MID:0x%04x, Token:0x%02x%02x (len %u), payload_len:%u",
		uri_path, code, code / 32u, code & 0x1f,
		type, coap_types[type], message_id,
		token[1], token[0], token_len, payload_len);

	if (payload && payload_len) {
		LOG_HEXDUMP_DBG(payload, payload_len, "payload");
	}

	/* Look for a record related to this message, and remove if found */
	err = find_response(token, token_len, message_id, code, type, payload_len, &cb, &user);
	if (err < 0) {
		LOG_ERR("No match for message and token");
		LOG_INF("Sending RESET to server");
		err = client_response(&reply, NULL, message_id, 0, NULL, false);
		if (err) {
			goto done;
		}
	} else if (err > 0) {
		LOG_DBG("Looking for and removing 2nd entry");
		err = find_response(token, token_len, message_id, code, type, 0, &cb, &user);
		if (err) {
			goto done;
		}
	} else if (type == COAP_TYPE_CON) {
		LOG_INF("ACKing a CON from server");
		err = client_response(&reply, uri_path, message_id, token_len, token, true);
		if (err) {
			goto done;
		}
	}

	int block2 = coap_get_option_int(&reply, COAP_OPTION_BLOCK2);
	int size2 = coap_get_option_int(&reply, COAP_OPTION_SIZE2);
	bool last_block = !GET_MORE(block2);
	int block_num = GET_BLOCK_NUM(block2);
	int block_size = coap_block_size_to_bytes(GET_BLOCK_SIZE(block2));

	if (block2 > 0) {
		LOG_INF("BLOCK TRANSFER: total size:%d, block num:%d, block_size:%d, last_block:%d",
			size2, block_num, block_size, last_block);
	}

	count = ARRAY_SIZE(options) - 1;
	count = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT,
				   options, count);
	if (count > 1) {
		LOG_ERR("Unexpected number of content format options: %d", count);
		err = -EINVAL;
		goto done;
	} else if (count == 1) {
		if (options[0].len == 0) {
			format = COAP_CONTENT_FORMAT_TEXT_PLAIN;
		} else if (options[0].len > 1) {
			LOG_ERR("Unexpected content format length: %d", options[0].len);
			err = -EINVAL;
			goto done;
		} else {
			format = options[0].value[0];
		}
	}

	int i;

	for (i = 0; coap_content_types[i].name != NULL; i++) {
		if (coap_content_types[i].type == format) {
			LOG_INF("Content format: %s (%d)", coap_content_types[i].name,
				format);
			break;
		}
	}
	if (coap_content_types[i].type != format) { /* not found */
		LOG_WRN("Undefined content format received: %d", format);
	}

	if (cb) {
		err = cb(payload, payload_len, format, user);
		if (err) {
			LOG_ERR("Error from callback:%d", err);
		}
	}

done:
	return err;
}


static void coap_thread_fn(void)
{
	int err;

	LOG_INF("Begin coap thread");
	k_sem_take(&ready_sem, K_FOREVER);

	LOG_INF("Start coap thread");
	for (;;) {
		err = client_receive(APP_COAP_RECEIVE_INTERVAL_MS);
		if (err) {
			LOG_ERR("Error from client_receive: %d", err);
		}
	}
}

K_THREAD_DEFINE(coap_thread, CONFIG_COAP_THREAD_STACK_SIZE, coap_thread_fn,
		NULL, NULL, NULL, 0, 0, 0);

/**@brief Send CoAP request. */
static int client_send(enum coap_method method, const char *resource, const char *query,
		       uint8_t *buf, size_t buf_len,
		       enum coap_content_format fmt_out,
		       enum coap_content_format fmt_in,
		       bool response_expected,
		       bool reliable,
		       coap_callback cb, void *user)
{
	int err;
	int i;
	int num;
	struct coap_packet request = {0};
	uint16_t message_id = coap_next_id();
	enum coap_msgtype msg_type = COAP_TYPE_CON; //reliable ? COAP_TYPE_CON : COAP_TYPE_NON_CON;
	struct nrf_cloud_coap_message *msg;
	bool cbor_fmt = (fmt_out == COAP_CONTENT_FORMAT_APP_CBOR);
	const char *method_name;

	if ((method >= 0) && (method <= COAP_METHOD_IPATCH)) {
		method_name = coap_methods[method];
	} else {
		LOG_ERR("Unknown method: %d", method);
		return -EINVAL;
	}

	next_token++;

	LOG_INF("%s %s%c%s, type:%d %s, contenttype:%d, accept:%d, payload:%s, len:%ld",
		method_name, resource, query ? '?' : ' ', query ? query : "",
		msg_type, coap_types[msg_type], fmt_out, fmt_in,
		cbor_fmt ? "" : (const char *)buf, (long)buf_len);
	if (cbor_fmt) {
		LOG_HEXDUMP_INF(buf, buf_len, "CBOR");
	}
	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       sizeof(next_token), (uint8_t *)&next_token,
			       method, message_id);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP request: %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					(uint8_t *)resource,
					strlen(resource));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP URI option: %d", err);
		return err;
	}

	if (buf && buf_len) {
		err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
						&fmt_out, sizeof(fmt_out));
		if (err < 0) {
			LOG_ERR("Failed to encode CoAP content format option: %d", err);
			return err;
		}
	}

	if (query) {
		err = coap_packet_append_option(&request, COAP_OPTION_URI_QUERY,
						query, strlen(query));
		if (err < 0) {
			LOG_ERR("Failed to encode query: %d", err);
		}
	}

	if (response_expected) {
		err = coap_packet_append_option(&request, COAP_OPTION_ACCEPT,
						&fmt_in, sizeof(fmt_in));
		if (err < 0) {
			LOG_ERR("Failed to encode accept option: %d", err);
			return err;
		}
	}

	if (buf && buf_len) {
		err = coap_packet_append_payload_marker(&request);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload marker: %d", err);
			return err;
		}

		err = coap_packet_append_payload(&request, buf, buf_len);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload: %d", err);
			return err;
		}
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, errno %d, err %d, sock %d, "
			"request.data %p, request.offset %u",
			-errno, err, sock, request.data, request.offset);
		return -errno;
	}

	num = (msg_type == COAP_TYPE_CON) ? 1 : 0;
	num += response_expected  ? 1 : 0;
	for (i = 0; i < num; i++) {
		msg = k_malloc(sizeof(struct nrf_cloud_coap_message));
		if (msg) {
			sys_dnode_init(&msg->node);
			msg->message_id = message_id;
			memcpy(msg->token, &next_token, sizeof(next_token));
			msg->token_len = sizeof(next_token);
			msg->cb = cb;
			msg->user = user;
			sys_dlist_append(&con_messages, &msg->node);
			num_con_messages++;
			LOG_DBG("Added MID:0x%04x, Token:0x%04x to list; len:%d",
				message_id, next_token, num_con_messages);
		}
	}

	LOG_INF("CoAP %s: MID:0x%04x, Token:0x%04x",
		method_name, message_id, next_token);

	return 0;
}

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt_out,
		    enum coap_content_format fmt_in,
		    coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_GET, resource, query,
			   buf, len, fmt_out, fmt_in, true, false, cb, user);
}

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt,
		    coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_POST, resource, query,
			   buf, len, fmt, fmt, false, false, cb, user);
}

/**@brief Send CoAP PUT request. */
int client_put_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt,
		    coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_PUT, resource, query,
			   buf, len, fmt, fmt, false, false, cb, user);
}

/**@brief Send CoAP DELETE request. */
int client_delete_send(const char *resource, const char *query,
		       uint8_t *buf, size_t len,
		       enum coap_content_format fmt,
		       coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_DELETE, resource, query,
			   buf, len, fmt, fmt, false, true, cb, user);
}

/**@brief Send CoAP FETCH request. */
int client_fetch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt_out,
		      enum coap_content_format fmt_in,
		      coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_FETCH, resource, query,
			   buf, len, fmt_out, fmt_in, true, true, cb, user);
}

/**@brief Send CoAP PATCH request. */
int client_patch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt,
		      coap_callback cb, void *user)
{
	return client_send(COAP_METHOD_PATCH, resource, query,
			   buf, len, fmt, fmt, false, true, cb, user);
}

int client_close(void)
{
	return close(sock);
}

