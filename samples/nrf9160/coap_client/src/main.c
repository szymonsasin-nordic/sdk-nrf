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
#include <modem/modem_key_mgmt.h>
#include <modem/modem_info.h>
#include <random/rand32.h>
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <mbedtls/ssl.h>
#include <net/tls_credentials.h>
#include <nrf_socket.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(app_coap_client, CONFIG_APP_LOG_LEVEL);

#define APP_COAP_SEND_INTERVAL_MS 5000
#define APP_COAP_INTERVAL_LIMIT 360
#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

/* Uncomment to limit cipher negotation to a list */
#define RESTRICT_CIPHERS

/* Uncomment to incrementally increase time between coap packets */
#define DELAY_INTERPACKET_PERIOD

/* Uncomment to display the cipherlist available */
/* #define DUMP_CIPHERLIST */

/* Which security tag to use */
#define SECTAG 1

static int sock;
static struct pollfd fds;
static struct sockaddr_storage server;
static uint16_t next_token;
static sec_tag_t sectag = SECTAG;

static uint8_t coap_buf[APP_COAP_MAX_MSG_LEN];

#if !defined(CONFIG_NET_SOCKETS_OFFLOAD_TLS)
mbedtls_ssl_context ssl_context;
mbedtls_ssl_config ssl_config;
#endif

static struct connection_info
{
	uint8_t s4_addr[4];
	uint8_t d4_addr[4];
} connection_info;

static struct modem_param_info mdm_param;

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

static int provision_psk(void)
{
	int ret;
	const char *identity = "n3526561061234690123456789abcdef";
	uint16_t identity_len;
	const char *psk = "000102030405060708090a0b0c0d0e0f";
	uint16_t psk_len;

	identity_len = strlen(identity);
	psk_len = strlen(psk);

	LOG_DBG("psk identity: %s len %u", log_strdup(identity), identity_len);
	LOG_HEXDUMP_DBG(psk, psk_len, "psk");

#if defined(CONFIG_NET_SOCKETS_OFFLOAD_TLS)
	char psk_hex[64];

	/* Convert PSK to a format accepted by the modem. */
	psk_len = bin2hex(psk, psk_len, psk_hex, sizeof(psk_hex));
	if (psk_len == 0) {
		LOG_ERR("PSK is too large to convert (%d)", -EOVERFLOW);
		return -EOVERFLOW;
	}
	LOG_HEXDUMP_INF(psk_hex, 64, "psk_hex");

	lte_lc_offline();

	ret = modem_key_mgmt_write(SECTAG, MODEM_KEY_MGMT_CRED_TYPE_PSK, psk_hex, psk_len);
	if (ret < 0) {
		LOG_ERR("Setting cred tag %d type %d failed (%d)", SECTAG,
			(int)MODEM_KEY_MGMT_CRED_TYPE_PSK, ret);
		goto exit;
	}

	ret = modem_key_mgmt_write(SECTAG, MODEM_KEY_MGMT_CRED_TYPE_IDENTITY, identity,
				   identity_len);
	if (ret < 0) {
		LOG_ERR("Setting cred tag %d type %d failed (%d)", SECTAG,
			(int)MODEM_KEY_MGMT_CRED_TYPE_IDENTITY, ret);
	}
exit:
	lte_lc_connect();
#else
	ret = tls_credential_add(sectag, TLS_CREDENTIAL_PSK, psk, psk_len);
	if (ret) {
		LOG_ERR("Failed to set PSK: %d", ret);
	}
	ret = tls_credential_add(sectag, TLS_CREDENTIAL_PSK_ID, identity, identity_len);
	if (ret) {
		LOG_ERR("Failed to set PSK identity: %d", ret);
	}
#endif
	return ret;
}

static int get_device_ip_address(void)
{
	int err;

	err = modem_info_init();
	if (err) {
		LOG_ERR("Could not init modem info: %d", err);
		return err;
	}
	err = modem_info_params_init(&mdm_param);
	if (err) {
		LOG_ERR("Could not get modem info: %d", err);
		return err;
	}

	err = modem_info_params_get(&mdm_param);
	if (err) {
		LOG_ERR("Could not get modem params: %d", err);
		return err;
	}
	printk("Application Name:        %s\n", mdm_param.device.app_name);
	printk("nRF Connect SDK version: %s\n", mdm_param.device.app_version);
	printk("Modem FW version:        %s\n", mdm_param.device.modem_fw.value_string);
	printk("IMEI:                    %s\n", mdm_param.device.imei.value_string);
	printk("Device IPv4 Address:     %s\n", mdm_param.network.ip_address.value_string);

	err = inet_pton(AF_INET, mdm_param.network.ip_address.value_string, connection_info.d4_addr);
	if (err == 1) {
		return 0;
	}
	return errno;
}

/**@brief Initialize the CoAP client */
static int client_init(void)
{
	int err;
#if defined(RESTRICT_CIPHERS)
	static const int ciphers[] = {
		MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8, 0
	};
#endif

	printk("Provisioning PSK\n");
	err = provision_psk();
	if (err) {
		return err;
	}

	LOG_DBG("Creating socket");
#if defined(CONFIG_NET_SOCKETS_OFFLOAD_TLS)
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
#else
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NATIVE_TLS, IPPROTO_DTLS_1_2);
#endif
	if (sock < 0) {
		LOG_ERR("Failed to create CoAP socket: %d.", errno);
		return -errno;
	}

	LOG_INF("Setting socket options");

	err = setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_COAP_SERVER_HOSTNAME,
			 sizeof(CONFIG_COAP_SERVER_HOSTNAME));
	if (err) {
		LOG_ERR("Error setting hostname: %d", errno);
	}

	err = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, &sectag, sizeof(sec_tag_t));
	if (err) {
		LOG_ERR("Error setting sectag list: %d", errno);
	}

#if defined(RESTRICT_CIPHERS)
	err = setsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, ciphers, sizeof(ciphers));
	if (err) {
		LOG_ERR("Error setting cipherlist: %d", errno);
	}
#endif
#if defined(DUMP_CIPHERLIST)
	int len;
	int old_ciphers[32];

	len = sizeof(old_ciphers);
	err = getsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, old_ciphers, &len);
	if (err) {
		LOG_ERR("Error getting cipherlist: %d", errno);
	} else {
		int count = len / sizeof(int);

		printk("New cipherlist:\n");
		for (int i = 0; i < count; i++) {
			printk("%d. 0x%04X = %s\n", i, (unsigned int)old_ciphers[i],
			       mbedtls_ssl_get_ciphersuite_name(old_ciphers[i]));
		}
	}
#endif

	err = get_device_ip_address();
	if (err) {
		return err;
	}
	printk("server addr %u.%u.%u.%u, client addr %u.%u.%u.%u\n",
	       connection_info.s4_addr[0], connection_info.s4_addr[1],
	       connection_info.s4_addr[2], connection_info.s4_addr[3],
	       connection_info.d4_addr[0], connection_info.d4_addr[1],
	       connection_info.d4_addr[2], connection_info.d4_addr[3]);

#if defined(CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID)
	uint8_t dummy;

	err = setsockopt(sock, SOL_TLS, TLS_DTLS_CONNECTION_ID, &dummy, 0);
	if (err) {
		LOG_ERR("Error setting connection ID: %d", errno);
	}
#endif

	err = connect(sock, (struct sockaddr *)&server,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Connect failed : %d", errno);
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

static int client_print_connection_id(void)
{
	int err = 0;
#if defined(CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID)
	static struct tls_dtls_peer_cid last_cid;
	struct tls_dtls_peer_cid cid;
	int cid_len = 0;
	int i;

	cid_len = sizeof(cid);
	memset(&cid, 0, cid_len);
	err = getsockopt(sock, SOL_TLS, TLS_DTLS_CONNECTION_ID, &cid, &cid_len);
	if (!err) {
		if (last_cid.enabled != cid.enabled) {
			LOG_WRN("CID ENABLE CHANGED");
		}
		if (last_cid.peer_cid_len != cid.peer_cid_len) {
			LOG_WRN("CID LEN CHANGED from %d to %d", last_cid.peer_cid_len,
			       cid.peer_cid_len);
			if (cid.peer_cid_len) {
				printk("DTLS CID IS  enabled:%d, len:%d ", cid.enabled,
				       cid.peer_cid_len);
				for (i = 0; i < cid.peer_cid_len; i++) {
					printk("0x%02x ", cid.peer_cid[i]);
				}
				printk("\n");
			}
		} else {
			if (memcmp(last_cid.peer_cid, cid.peer_cid, cid.peer_cid_len) != 0) {
				LOG_WRN("CID CHANGED!");

				printk("DTLS CID WAS enabled:%d, len:%d ", last_cid.enabled,
				       last_cid.peer_cid_len);
				for (i = 0; i < last_cid.peer_cid_len; i++) {
					printk("0x%02x ", last_cid.peer_cid[i]);
				}
				printk("\n");
			}
			printk("DTLS CID IS  enabled:%d, len:%d ", cid.enabled,
			       cid.peer_cid_len);
			for (i = 0; i < cid.peer_cid_len; i++) {
			printk("0x%02x ", cid.peer_cid[i]);
			}
			printk("\n");
		}
		memcpy(&last_cid, &cid, sizeof(last_cid));
	} else {
		LOG_ERR("Unable to get connection ID: %d", -errno);
	}
#endif
	return err;
}

/**@brief Handles responses from the remote CoAP server. */
static int client_handle_get_response(uint8_t *buf, int received)
{
	int err;
	struct coap_packet reply;
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t token[8];
	uint16_t token_len;
	uint8_t temp_buf[16];

	err = coap_packet_parse(&reply, buf, received, NULL, 0);
	if (err < 0) {
		LOG_ERR("Malformed response received: %d", err);
		return err;
	}

	payload = coap_packet_get_payload(&reply, &payload_len);
	token_len = coap_header_get_token(&reply, token);

	if ((token_len != sizeof(next_token)) ||
	    (memcmp(&next_token, token, sizeof(next_token)) != 0)) {
		LOG_ERR("Invalid token received: 0x%02x%02x, expected: 0x%04x",
		       token[1], token[0], next_token);
		return 0;
	}

	if (payload_len > 0) {
		snprintf(temp_buf, MIN(payload_len + 1, sizeof(temp_buf)), "%s", payload);
	} else {
		strcpy(temp_buf, "EMPTY");
	}

	printk("CoAP response: code: 0x%x, token 0x%02x%02x, payload: %s\n",
	       coap_header_get_code(&reply), token[1], token[0], temp_buf);

	return 0;
}

/**@biref Send CoAP GET request. */
static int client_get_send(void)
{
	int err;
	struct coap_packet request;
	const char *resource = "command?type=firmware";

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, COAP_TYPE_NON_CON,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_GET, coap_next_id());
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

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, %d", errno);
		return -errno;
	}

	printk("CoAP request sent: token 0x%04x\n", next_token);
	client_print_connection_id();

	return 0;
}

/**@biref Send CoAP PUT request. */
static int client_put_send(void)
{
	int err;
	struct coap_packet request;
	const char *resource = "telemetry";
	const char *payload = "deadbeef";

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, COAP_TYPE_NON_CON,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_PUT, coap_next_id());
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

	err = coap_packet_append_payload_marker(&request);
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload marker, %d", err);
		return err;
	}

	err = coap_packet_append_payload(&request, payload, strlen(payload));
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload, %d", err);
		return err;
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, %d", errno);
		return -errno;
	}

	printk("CoAP request sent: token 0x%04x\n", next_token);
	client_print_connection_id();

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

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
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
		printk("Waitng for carrier registration...\n");
		k_sem_take(&carrier_registered, K_FOREVER);
		printk("Registered!\n");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

		printk("LTE Link Connecting ...\n");
		err = lte_lc_init_and_connect();
		__ASSERT(err == 0, "LTE link could not be established.");
		printk("LTE Link Connected!\n");
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
		LOG_ERR("poll error: %d", errno);
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

void main(void)
{
	int64_t next_msg_time = APP_COAP_SEND_INTERVAL_MS;
	int err;
	int received;
	int i = 1;
	bool do_send = false;

	printk("The nRF CoAP client sample started\n");

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
			if (do_send) {
				printk("\n%d. Calling client_put_send()\n", i);
				if (client_put_send() != 0) {
					printk("Failed to send PUT request, exit...\n");
					break;
				}
			} else {
				printk("\n%d. Calling client_get_send()\n", i);
				if (client_get_send() != 0) {
					printk("Failed to send GET request, exit...\n");
					break;
				}
			}
		        do_send = !do_send;
			next_msg_time += APP_COAP_SEND_INTERVAL_MS * i;
#if defined(DELAY_INTERPACKET_PERIOD)
			if (++i > APP_COAP_INTERVAL_LIMIT) {
				i = APP_COAP_INTERVAL_LIMIT;
			}
#endif
		}

		int64_t remaining = next_msg_time - k_uptime_get();

		if (remaining < 0) {
			remaining = 0;
		}

		err = wait(remaining);
		if (err < 0) {
			if (err == -EAGAIN) {
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
		err = client_handle_get_response(coap_buf, received);
		if (err < 0) {
			printk("Invalid response, exit...\n");
			break;
		}
	}

	(void)close(sock);
}
