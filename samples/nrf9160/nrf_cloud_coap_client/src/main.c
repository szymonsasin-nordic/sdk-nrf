/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr.h>
#include <net/coap.h>
#include <net/socket.h>
#include <modem/lte_lc.h>
#include <random/rand32.h>
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <tinycbor/cbor.h>
#include <tinycbor/cbor_buf_reader.h>
#include <tinycbor/cbor_buf_writer.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define APP_COAP_SEND_INTERVAL_MS 300000
#define APP_COAP_RECEIVE_INTERVAL_MS 5000
#define APP_COAP_CLOSE_THRESHOLD_MS 4000
#define APP_COAP_CONNECTION_CHECK_MS 5000
#define APP_COAP_INTERVAL_LIMIT 1
#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

/* Uncomment to enable sending cell_pos parameters with GET as payload */
#define CELL_POS_PAYLOAD

/* Uncomment to limit cipher negotation to a list */
#define RESTRICT_CIPHERS

/* Uncomment to incrementally increase time between coap packets */
#define DELAY_INTERPACKET_PERIOD

/* Open and close socket every cycle */
/* #define OPEN_AND_SHUT */

/* Uncomment to display the cipherlist available */
/* #define DUMP_CIPHERLIST */

static int sock;
static struct pollfd fds;
static struct sockaddr_storage server;
static uint16_t next_token;

static uint8_t coap_buf[APP_COAP_MAX_MSG_LEN];

enum nrf_cloud_coap_response
{
	NRF_CLOUD_COAP_NONE,
	NRF_CLOUD_COAP_LOCATION,
	NRF_CLOUD_COAP_PGPS,
	NRF_CLOUD_COAP_FOTA_JOB
};

static struct connection_info
{
	uint8_t s4_addr[4];
	uint8_t d4_addr[4];
} connection_info;

static sys_dlist_t con_messages;
static int num_con_messages;

struct nrf_cloud_coap_message {
	sys_dnode_t node;
	uint16_t message_id;
	uint16_t token_len;
	uint8_t token[8];
};

static const char *coap_types[] =
{
	"CON", "NON", "ACK", "RST", NULL
};

int client_print_connection_id(int sock, bool verbose);
int client_dtls_init(int sock);

#if defined(CONFIG_NRF_MODEM_LIB)
/**@brief Recoverable modem library error. */
void nrf_modem_recoverable_error_handler(uint32_t err)
{
	printk("Modem library recoverable error: %u\n", (unsigned int)err);
}
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

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

	err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
	if (err != 0) {
		LOG_ERR("ERROR: getaddrinfo failed %d", err);
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
	printk("IPv4 Address found %s\n", ipv4_addr);

	/* Free the address. */
	freeaddrinfo(result);

	return 0;
}

/**@brief Initialize the CoAP client */
static int client_init(void)
{
	int err;

	LOG_DBG("Creating socket");
#if !defined(CONFIG_COAP_DTLS)
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
#if defined(CONFIG_NET_SOCKETS_OFFLOAD_TLS)
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
#else
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NATIVE_TLS, IPPROTO_DTLS_1_2);
#endif
#endif
	if (sock < 0) {
		LOG_ERR("Failed to create CoAP socket: %d.", -errno);
		return -errno;
	}

#if defined(CONFIG_COAP_DTLS)
	err = client_dtls_init(sock);
#endif
	err = connect(sock, (struct sockaddr *)&server,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Connect failed : %d", -errno);
		return -errno;
	} else {
		printk("Connect succeeded.\n");
	}

	/* Initialize FDS, for poll. */
	fds.fd = sock;
	fds.events = POLLIN;

	/* Randomize token. */
	next_token = sys_rand32_get();

	return 0;
}

static int decode_cbor_response(enum nrf_cloud_coap_response response,
				const uint8_t *payload, uint16_t payload_len,
				char *temp_buf, size_t temp_size)
{
	struct CborParser parser;
	struct cbor_buf_reader reader;
	CborError err_cbor;
	CborType type_cbor;
	CborValue value;
	CborValue map_value;
	int map_len = 0;
	char fulfilled_with[11] = {0};
	size_t len;
	double lat;
	double lon;
	double uncertainty;

	cbor_buf_reader_init(&reader, payload, payload_len);
	err_cbor = cbor_parser_init(&reader.r, 0, &parser, &value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error parsing response: %d", err_cbor);
		return -EIO;
	}

	type_cbor = cbor_value_get_type(&value);
	if (type_cbor != CborArrayType) {
		LOG_ERR("Expected CBOR array; got %d", (int)type_cbor);
		return -EBADMSG;
	}

	/* TODO: use response to look up entry in array of
	 * structs that indicate expected type for each value,
	 * then uses that to decode
	 */
	err_cbor = cbor_value_get_array_length(&value, &map_len);
	if ((err_cbor != CborNoError) || (map_len != 4)) {
		LOG_ERR("Error getting array length: %d", err_cbor);
		return -EBADMSG;
	}

	/* Enter the array */
	err_cbor = cbor_value_enter_container(&value, &map_value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error entering array: %d", err_cbor);
		return -EACCES;
	}

	len = sizeof(fulfilled_with) - 1;
	err_cbor = cbor_value_copy_text_string(&map_value, fulfilled_with,
					       &len, &map_value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting fulfilled_with: %d", err_cbor);
		return -EBADMSG;
	}
	fulfilled_with[len] = '\0';

	err_cbor = cbor_value_get_double(&map_value, &lat);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting lat: %d", err_cbor);
		return -EBADMSG;
	}

	err_cbor = cbor_value_get_double(&map_value, &lon);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting lon: %d", err_cbor);
		return -EBADMSG;
	}

	err_cbor = cbor_value_get_double(&map_value, &uncertainty);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting uncertainty: %d", err_cbor);
		return -EBADMSG;
	}

	/* Exit array */
	err_cbor = cbor_value_leave_container(&value, &map_value);
	if (err_cbor != CborNoError) {
		return -EACCES;
	}

	snprintf(temp_buf, temp_size - 1, "fulfilledWith:%s, lat:%g, lon:%g, unc:%g\n",
	       fulfilled_with, lat, lon, uncertainty);

	return 0;
}

static int encode_cbor_sensor(double value, int64_t ts, uint8_t *buf, size_t *len)
{
	struct cbor_buf_writer wr_encoder;
	struct CborEncoder encoder;
	CborEncoder array_encoder;

	cbor_buf_writer_init(&wr_encoder, buf, *len);
	cbor_encoder_init(&encoder, &wr_encoder.enc, 0);

	cbor_encoder_create_array(&encoder, &array_encoder, 2);
	cbor_encode_double(&array_encoder, value);
	cbor_encode_uint(&array_encoder, ts);
	cbor_encoder_close_container(&encoder, &array_encoder);

	*len = cbor_buf_writer_buffer_size(&wr_encoder, buf);
	LOG_HEXDUMP_DBG(buf, *len, "CBOR TEMP");

	return 0;
}

#if defined(CELL_POS_PAYLOAD)
static int encode_cbor_cell_pos(bool do_reply, unsigned int mcc, unsigned int mnc, unsigned int eci,
				unsigned int tac, float rsrp, float rsrq, unsigned int earfcn,
				uint8_t *buf, size_t *len)
{
	struct cbor_buf_writer wr_encoder;
	struct CborEncoder encoder;
	CborEncoder array_encoder;

	cbor_buf_writer_init(&wr_encoder, buf, *len);
	cbor_encoder_init(&encoder, &wr_encoder.enc, 0);

	cbor_encoder_create_array(&encoder, &array_encoder, 8);
	cbor_encode_uint(&array_encoder, eci);
	cbor_encode_uint(&array_encoder, mcc);
	cbor_encode_uint(&array_encoder, mnc);
	cbor_encode_uint(&array_encoder, tac);
	cbor_encode_float(&array_encoder, rsrp);
	cbor_encode_float(&array_encoder, rsrq);
	cbor_encode_uint(&array_encoder, earfcn);
	cbor_encode_boolean(&array_encoder, do_reply);
	cbor_encoder_close_container(&encoder, &array_encoder);

	*len = cbor_buf_writer_buffer_size(&wr_encoder, buf);
	LOG_HEXDUMP_DBG(buf, *len, "CBOR CELL_POS");

	return 0;
}
#endif

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

	printk("CoAP %s response sent: token 0x%04x\n", coap_types[msg_type], next_token);

	return 0;
}

/**@brief Handles responses from the remote CoAP server. */
static int client_handle_get_response(enum nrf_cloud_coap_response expected_response,
				      uint8_t *buf, int received)
{
	int err;
	struct coap_packet reply;
	struct coap_option options[16] = {};
	int count;
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t token[8] = {0};
	uint16_t token_len;
	uint8_t temp_buf[100];
	char uri_path[64];
	enum coap_content_format format = 0;
	uint16_t message_id;
	uint8_t code;
	uint8_t type;
	struct nrf_cloud_coap_message *msg = NULL;

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

	LOG_INF("Got response uri:%s, code:0x%02x, type:%u %s, MID:%u, token:0x%02x%02x (len %u)",
		log_strdup(uri_path), code, type, coap_types[type], message_id,
		token[1], token[0], token_len);

	err = -ENOMSG;
	SYS_DLIST_FOR_EACH_CONTAINER(&con_messages, msg, node) {
		if ((type == COAP_TYPE_CON) || (type == COAP_TYPE_NON_CON)) {
			/* match token only */
			if ((token_len == msg->token_len) &&
			    (memcmp(msg->token, token, token_len) == 0)) {
				LOG_INF("Matched token");
				err = 0;
				break;
			}
		} else { /* ACK or RESET */
			/* EMPTY responses: match MID only */
			if (code == 0) { 
				if (msg->message_id == message_id) {
					LOG_INF("Matched empty %s.",
						(type == COAP_TYPE_ACK) ? "ACK" : "RESET");
					err = 0;
					break;
				}
			} else {
				if ((msg->message_id == message_id) &&
				    (token_len == msg->token_len) &&
				    (memcmp(msg->token, token, token_len) == 0)) {
					LOG_INF("Found MID and token");
					err = 0;
					break;
				}
			}
		}
	}
	if (err) {
		LOG_ERR("No match for message and token");
		/* TODO: sent RESET? */
		return err;
	}

	if (type == COAP_TYPE_CON) {
		LOG_INF("ACKing a CON from server");
		err = client_response(&reply, uri_path, message_id, token_len, token, true);
		if (err) {
			goto done;
		}
	}

	count = ARRAY_SIZE(options) - 1;
	count = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT,
				   options, count);
	if (count > 1) {
		LOG_ERR("Unexpected number of content format options: %d", count);
		err = -EINVAL;
		goto done;
	} else if (count == 1) {
		if (options[0].len != 1) {
			LOG_ERR("Unexpected content format length: %d", options[0].len);
			err = -EINVAL;
			goto done;
		}
		format = options[0].value[0];
		printk("Content format: %d\n", format);
	}

	payload = coap_packet_get_payload(&reply, &payload_len);
	if (payload_len > 0) {
		if (format == COAP_CONTENT_FORMAT_APP_CBOR) {
			err = decode_cbor_response(expected_response, payload, payload_len,
						   temp_buf, sizeof(temp_buf));
			if (err) {
				goto done;
			}
		} else {
			snprintf(temp_buf, MIN(payload_len + 1, sizeof(temp_buf)), "%s", payload);
			printk("CoAP payload: %s\n", temp_buf);
		}
	} else {
		printk("CoAP payload: EMPTY\n");
	}

done:
	if (msg != NULL) {
		sys_dlist_remove(&msg->node);
		k_free(msg);
		num_con_messages--;
		LOG_INF("messages left:%d", num_con_messages);
	}
	return err;
}

/**@brief Send CoAP GET request. */
static int client_get_send(const char *resource, uint8_t *buf, size_t len)
{
	int err;
	int i;
	int num;
	struct coap_packet request;
	uint8_t format = COAP_CONTENT_FORMAT_APP_CBOR;
	uint16_t message_id = coap_next_id();
	enum coap_msgtype msg_type = COAP_TYPE_CON;
	struct nrf_cloud_coap_message *msg;

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_GET, message_id);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP request, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					(uint8_t *)resource,
					strlen(resource));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP option, %d", err);
		return err;
	}

	if (buf) {
		err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
						&format, sizeof(format));
		if (err < 0) {
			LOG_ERR("Failed to encode CoAP content format option, %d", err);
			return err;
		}

		err = coap_packet_append_payload_marker(&request);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload marker, %d", err);
			return err;
		}

		err = coap_packet_append_payload(&request, buf, len);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload, %d", err);
			return err;
		}
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, %d", -errno);
		return -errno;
	}

	num = (msg_type == COAP_TYPE_CON) ? 2 : 1;
	for (i = 0; i < num; i++) {
		msg = k_malloc(sizeof(struct nrf_cloud_coap_message));
		if (msg) {
			sys_dnode_init(&msg->node);
			msg->message_id = message_id;
			memcpy(msg->token, &next_token, sizeof(next_token));
			msg->token_len = sizeof(next_token);
			sys_dlist_append(&con_messages, &msg->node);
			num_con_messages++;
			LOG_INF("Added MID:%u, token:0x%04x to list; len:%d",
				message_id, next_token, num_con_messages);
		}
	}
	printk("CoAP request sent: MID:%u, token:0x%04x\n", message_id, next_token);
	//client_print_connection_id(sock, true);

	return 0;
}

/**@biref Send CoAP PUT request. */
static int client_put_send(const char *resource, uint8_t *buf, size_t buf_len)
{
	int err;
	struct coap_packet request;
	uint8_t format = COAP_CONTENT_FORMAT_APP_CBOR;
	uint16_t message_id = coap_next_id();
	enum coap_msgtype msg_type = COAP_TYPE_CON;
	struct nrf_cloud_coap_message *msg;

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_PUT, message_id);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP request, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					(uint8_t *)resource,
					strlen(resource));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP URI option, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
					&format, sizeof(format));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP content format option, %d", err);
		return err;
	}

	err = coap_packet_append_payload_marker(&request);
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload marker, %d", err);
		return err;
	}

	err = coap_packet_append_payload(&request, buf, buf_len);
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload, %d", err);
		return err;
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, %d", -errno);
		return -errno;
	}

	if (msg_type == COAP_TYPE_CON) {
		/* we expect an ACK back from the server */
		msg = k_malloc(sizeof(struct nrf_cloud_coap_message));
		if (msg) {
			sys_dnode_init(&msg->node);
			msg->message_id = message_id;
			memcpy(msg->token, &next_token, sizeof(next_token));
			msg->token_len = sizeof(next_token);
			sys_dlist_append(&con_messages, &msg->node);
			num_con_messages++;
			LOG_INF("Added MID:%u, token:0x%04x to list; len:%d",
				message_id, next_token, num_con_messages);
		}
	}

	printk("CoAP request sent: MID:%u, token:0x%04x\n", message_id, next_token);
	//client_print_connection_id(sock, true);

	return 0;
}

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);

void lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
	switch (event->type) {
	case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
		printk("LWM2M_CARRIER_EVENT_BSDLIB_INIT\n");
		break;
	case LWM2M_CARRIER_EVENT_CONNECT:
		printk("LWM2M_CARRIER_EVENT_CONNECT\n");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECT:
		printk("LWM2M_CARRIER_EVENT_DISCONNECT\n");
		break;
	case LWM2M_CARRIER_EVENT_READY:
		printk("LWM2M_CARRIER_EVENT_READY\n");
		k_sem_give(&carrier_registered);
		break;
	case LWM2M_CARRIER_EVENT_FOTA_START:
		printk("LWM2M_CARRIER_EVENT_FOTA_START\n");
		break;
	case LWM2M_CARRIER_EVENT_REBOOT:
		printk("LWM2M_CARRIER_EVENT_REBOOT\n");
		break;
	}
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE network");
			k_sem_give(&lte_ready);
		} else {
			LOG_INF("reg status %d", evt->nw_reg_status);
		}
		break;

	default:
		LOG_INF("LTE event %d (0x%x)", evt->type, evt->type);
		break;
	}
}


/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
	lte_lc_register_handler(lte_handler);
#if defined(CONFIG_LTE_LINK_CONTROL)
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
#if defined(CONFIG_LWM2M_CARRIER)
		/* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
		printk("Waiting for carrier registration...\n");
		k_sem_take(&carrier_registered, K_FOREVER);
		printk("Registered!\n");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

		printk("LTE Link Connecting ...\n");
		err = lte_lc_init_and_connect();
		__ASSERT(err == 0, "LTE link could not be established.");
		k_sem_take(&lte_ready, K_FOREVER);
		printk("LTE Link Connected!\n");
		err = lte_lc_psm_req(true);
		if (err) {
			LOG_ERR("Unable to enter PSM mode: %d", err);
		}

		err = nrf_modem_at_printf("AT+CEREG=5");
		if (err) {
			LOG_ERR("Can't subscribe to +CEREG events.");
		}

#endif /* defined(CONFIG_LWM2M_CARRIER) */
	}
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */
}

/* Returns 0 if data is available.
 * Returns -EAGAIN if timeout occured and there is no data.
 * Returns other, negative error code in case of poll error.
 */
static int wait(int timeout)
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

static void check_connection(void)
{
	static int64_t next_keep_serial_alive_time;
	char scratch_buf[256];
	int err;

	if (k_uptime_get() >= next_keep_serial_alive_time) {
		next_keep_serial_alive_time = k_uptime_get() + APP_COAP_CONNECTION_CHECK_MS;
		scratch_buf[0] = '\0';
		err = nrf_modem_at_cmd(scratch_buf, sizeof(scratch_buf), "%s", "AT%XMONITOR");
		printk("%s", scratch_buf);
		if (err) {
			LOG_ERR("Error on at cmd: %d", err);
		}
		//client_print_connection_id(sock, true);
	}
}

void main(void)
{
	int64_t next_msg_time;
	int delta_ms = APP_COAP_SEND_INTERVAL_MS;
	int err;
	int received;
	int i = 1;
	bool reconnect = false;
	uint8_t buffer[100];
	size_t len;
	double temp = 21.5;
	int64_t ts;
	enum nrf_cloud_coap_response expected_response = NRF_CLOUD_COAP_LOCATION;
	uint8_t *get_payload;
#if defined(CELL_POS_PAYLOAD)
	const char *get_res = "cell_pos";
#else
	const char *get_res = "cell_pos?doReply=true&mmc=260&mcc=310"
		              "&eci=21858829&tac=333&rsrp=-157&rsrq=-34.5&earfcn=0";
#endif

	printk("The nRF Cloud CoAP client sample started\n");

	sys_dlist_init(&con_messages);

	modem_configure();

	if (server_resolve() != 0) {
		printk("Failed to resolve server name\n");
		return;
	}

	if (client_init() != 0) {
		printk("Failed to initialize CoAP client\n");
		return;
	}

	next_msg_time = k_uptime_get();

	while (1) {
		if (k_uptime_get() >= next_msg_time) {
			if (reconnect) {
				reconnect = false;
				printk("Going online\n");
				err = lte_lc_normal();
				if (err) {
					LOG_ERR("Error going online: %d", err);
				} else {
					k_sem_take(&lte_ready, K_FOREVER);
					if (client_init() != 0) {
						printk("Failed to initialize CoAP client\n");
						return;
					}
				}
			}

			ts = k_uptime_get();
			err = date_time_uptime_to_unix_time_ms(&ts);
			if (err) {
				LOG_ERR("Error converting time: %d", err);
			}

			len = sizeof(buffer);
			encode_cbor_sensor(temp, ts, buffer, &len);

			printk("\n%d. Calling client_put_send()\n", i);
			if (client_put_send("temp", buffer, len) != 0) {
				printk("Failed to send PUT request, exit...\n");
				break;
			}

#if defined(CELL_POS_PAYLOAD)
			len = sizeof(buffer);
			encode_cbor_cell_pos(true, 260, 310, 21858829, 333,
					     -157.0F, -34.5F, 0, buffer, &len);
			get_payload = buffer;
#else
			get_payload = NULL;
			len = 0;
#endif
			printk("\n%d. Calling client_get_send()\n", i);
			if (client_get_send(get_res, get_payload, len) != 0) {
				printk("Failed to send GET request, exit...\n");
				break;
			}
			delta_ms = APP_COAP_SEND_INTERVAL_MS * i;
			LOG_INF("Next transfer in %d minutes, %d seconds",
				delta_ms / 60000, (delta_ms / 1000) % 60);
			next_msg_time += delta_ms;
#if defined(DELAY_INTERPACKET_PERIOD)
			if (++i > APP_COAP_INTERVAL_LIMIT) {
				i = APP_COAP_INTERVAL_LIMIT;
			}
#endif
#if defined(CONFIG_COAP_DTLS)
			client_print_connection_id(sock, false);
#endif
		}

		if (reconnect) {
			k_sleep(K_MSEC(APP_COAP_RECEIVE_INTERVAL_MS));
			check_connection();
			continue;
		}
		err = wait(APP_COAP_RECEIVE_INTERVAL_MS);
		if (err < 0) {
			if (err == -EAGAIN) {
				check_connection();
				continue;
			}

			printk("Poll error, exit...\n");
			break;
		}

		printk("Calling recv()\n");
		received = recv(sock, coap_buf, sizeof(coap_buf), MSG_DONTWAIT);
		if (received < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				printk("socket EAGAIN\n");
				continue;
			} else {
				printk("Socket error, exit...\n");
				break;
			}
		}

		if (received == 0) {
			printk("Empty datagram\n");
			continue;
		}

		printk("Calling client_handle_get_response()\n");
		err = client_handle_get_response(expected_response, coap_buf, received);
		if (err < 0) {
			printk("Invalid response, exit...\n");
			break;
		}

#if defined(OPEN_AND_SHUT)
		if (delta_ms > APP_COAP_CLOSE_THRESHOLD_MS) {
			reconnect = true;
			err = close(sock);
			if (err) {
				LOG_ERR("Error closing socket: %d", err);
			} else {
				printk("Socket closed.\n");
			}
			printk("Going offline\n");
			err = lte_lc_offline();
			if (err) {
				LOG_ERR("Error going offline: %d", err);
			} else {
			}
		}
#endif
	}

	(void)close(sock);
}
