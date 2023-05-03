/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#if defined(CONFIG_MODEM_INFO)
#include <modem/modem_info.h>
#endif
#if defined(CONFIG_MBEDTLS)
#include <mbedtls/ssl.h>
#include <mbedtls/debug.h>
#endif
#if defined(CONFIG_TLS_CREDENTIALS)
#include <zephyr/net/tls_credentials.h>
#endif
#include <nrf_socket.h>
#include <nrf_modem_at.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dtls, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

/* #define ALL_CERTS */

static bool dtls_saved;

#if defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
/* Uncomment to limit cipher negotation to a list */
//#define RESTRICT_CIPHERS
/* Uncomment to display the cipherlist available */
/* #define DUMP_CIPHERLIST */
#endif

static int sectag = CONFIG_NRF_CLOUD_COAP_SEC_TAG;

#if defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
static const unsigned char ca_certificate[] = {
	#include "ca_cert.pem"
};

#if defined(ALL_CERTS)
static const unsigned char client_certificate[] = {
	#include "client_crt.pem"
};

static const unsigned char private_key[] = {
	#include "client_prv.pem"
};
#endif

#else
static const unsigned char ca_certificate[] = {
	#include "ca_cert.h"
};

#if defined(ALL_CERTS)
static const unsigned char client_certificate[] = {
	#include "client_cert.h"
};

static const unsigned char private_key[] = {
	#include "private_key.h"
};
#endif
#endif

#if defined(CONFIG_MODEM_INFO)
static struct modem_param_info mdm_param;

static int get_modem_info(void)
{
	int err;

	err = modem_info_string_get(MODEM_INFO_IMEI,
				    mdm_param.device.imei.value_string,
				    MODEM_INFO_MAX_RESPONSE_SIZE);
	if (err <= 0) {
		LOG_ERR("Could not get IMEI: %d", err);
		return err;
	}

	err = modem_info_string_get(MODEM_INFO_FW_VERSION,
				    mdm_param.device.modem_fw.value_string,
				    MODEM_INFO_MAX_RESPONSE_SIZE);
	if (err <= 0) {
		LOG_ERR("Could not get mfw ver: %d", err);
		return err;
	}

	LOG_INF("IMEI:                    %s", mdm_param.device.imei.value_string);
	LOG_INF("Modem FW version:        %s", mdm_param.device.modem_fw.value_string);

	return 0;
}

int get_device_id(char *buf, size_t len)
{
	int err;

	err = get_modem_info();
	if (err) {
		return err;
	}

	snprintf(buf, len, "nrf-%s", mdm_param.device.imei.value_string);
	return 0;
}

#endif /* CONFIG_MODEM_INFO */

static int get_device_ip_address(uint8_t *d4_addr)
{
	int err;

#if defined(CONFIG_MODEM_INFO)
	err = modem_info_init();
	if (err) {
		return err;
	}

	err = modem_info_string_get(MODEM_INFO_IP_ADDRESS,
				    mdm_param.network.ip_address.value_string,
				    MODEM_INFO_MAX_RESPONSE_SIZE);
	if (err <= 0) {
		LOG_ERR("Could not get IP addr: %d", err);
		return err;
	}
	err = inet_pton(AF_INET, mdm_param.network.ip_address.value_string, d4_addr);
	if (err == 1) {
		return 0;
	}
	return errno;
#else
	d4[0] = 0;
	d4[1] = 0;
	d4[2] = 0;
	d4[3] = 0;
	return 0;
#endif

}

#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_PSK)

int provision_psk(void)
{
	int ret;
	static char identity[120];
	uint16_t identity_len;
	const char *psk;
	uint16_t psk_len;

	LOG_INF("Provisioning PSK to sectag %u", sectag);

	/* get IMEI so we can construct the PSK identity */
	ret = get_modem_info();
	if (ret) {
		return ret;
	}

	if (strlen(CONFIG_COAP_DTLS_PSK_IDENTITY)) {
		strncpy(identity, CONFIG_COAP_DTLS_PSK_IDENTITY, sizeof(identity) - 1);
	} else {
		const char *identity_fmt = "n%s0123456789abcdef";

		snprintf(identity, sizeof(identity), identity_fmt,
			 mdm_param.device.imei.value_string);
	}

	if (strlen(CONFIG_COAP_DTLS_PSK_SECRET)) {
		psk = CONFIG_COAP_DTLS_PSK_SECRET;
	} else {
		psk = "000102030405060708090a0b0c0d0e0f";
	}

	identity_len = strlen(identity);

	LOG_DBG("psk identity: %s len %u", identity, identity_len);

	psk_len = strlen(psk);
	LOG_HEXDUMP_DBG(psk, psk_len, "psk");

#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	char psk_hex[65];

	/* Convert PSK to a format accepted by the modem. */
	psk_len = bin2hex(psk, psk_len, psk_hex, sizeof(psk_hex));
	if (psk_len == 0) {
		LOG_ERR("PSK is too large to convert (%d)", -EOVERFLOW);
		return -EOVERFLOW;
	}
	LOG_HEXDUMP_INF(psk_hex, 64, "psk_hex");

	lte_lc_offline();

	ret = modem_key_mgmt_write(sectag, MODEM_KEY_MGMT_CRED_TYPE_PSK, psk_hex, psk_len);
	if (ret < 0) {
		LOG_ERR("Setting cred tag %d type %d failed (%d)", sectag,
			(int)MODEM_KEY_MGMT_CRED_TYPE_PSK, ret);
		goto exit;
	}

	ret = modem_key_mgmt_write(sectag, MODEM_KEY_MGMT_CRED_TYPE_IDENTITY, identity,
				   identity_len);
	if (ret < 0) {
		LOG_ERR("Setting cred tag %d type %d failed (%d)", sectag,
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

#else

int provision_ca(void)
{
	int ret;

	/* get IMEI so we can construct the PSK identity */
	ret = get_modem_info();
	if (ret) {
		return ret;
	}

	LOG_INF("Updating CA cert");

#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	lte_lc_offline();

	ret = modem_key_mgmt_write(sectag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   ca_certificate, sizeof(ca_certificate) - 1);
	if (ret < 0) {
		LOG_ERR("Setting cred tag %d type %d failed (%d)", sectag,
			(int)MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, ret);
	}

	lte_lc_connect();

#else
	/* Load CA certificate */
	ret = tls_credential_add(sectag, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register CA certificate: %d", ret);
		goto exit;
	}

#if defined(ALL_CERTS)
		/* Load server/client certificate */
	ret = tls_credential_add(sectag,
				TLS_CREDENTIAL_SERVER_CERTIFICATE,
				client_certificate, sizeof(client_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register public cert: %d", ret);
		goto exit;
	}

	/* Load private key */
	ret = tls_credential_add(sectag, TLS_CREDENTIAL_PRIVATE_KEY,
				private_key, sizeof(private_key));
	if (ret < 0) {
		LOG_ERR("Failed to register private key: %d", ret);
		goto exit;
	}
#endif
exit:
#endif
	return ret;
}
#endif

int dtls_init(int sock)
{
	int err;
#if defined(RESTRICT_CIPHERS)
	static const int ciphers[] = {
		MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8, 0
	};
#endif

	uint8_t d4_addr[4];

	/* once connected, cache the connection info */
	dtls_saved = false;

	err = get_device_ip_address(d4_addr);
	if (!err) {
		LOG_INF("Client IP address: %u.%u.%u.%u", d4_addr[0], d4_addr[1], d4_addr[2], d4_addr[3]);
	}

	LOG_INF("Setting socket options:");

	LOG_INF("  hostname: %s", CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME);
	err = setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME,
			 sizeof(CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME));
	if (err) {
		LOG_ERR("Error setting hostname: %d", errno);
		return err;
	}

	LOG_INF("  sectag: %d", sectag);
	err = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, &sectag, sizeof(sectag));
	if (err) {
		LOG_ERR("Error setting sectag list: %d", errno);
		return err;
	}

#if defined(RESTRICT_CIPHERS)
	LOG_INF("  restrict ciphers");
	err = setsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, ciphers, sizeof(ciphers));
	if (err) {
		LOG_ERR("Error setting cipherlist: %d", errno);
		return err;
	}
#endif
#if defined(DUMP_CIPHERLIST)
	int len;
	int ciphers[32];

	len = sizeof(ciphers);
	err = getsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, ciphers, &len);
	if (err) {
		LOG_ERR("Error getting cipherlist: %d", errno);
	} else {
		int count = len / sizeof(int);

		LOG_INF("New cipherlist:");
		for (int i = 0; i < count; i++) {
			LOG_INF("%d. 0x%04X = %s", i, (unsigned int)ciphers[i],
			       mbedtls_ssl_get_ciphersuite_name(ciphers[i]));
		}
	}
#endif

#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_CID)
	int cid_option = TLS_DTLS_CID_SUPPORTED;

	LOG_INF("  Enable connection id:");
	err = setsockopt(sock, SOL_TLS, TLS_DTLS_CID, &cid_option, sizeof(cid_option));
	if (err) {
		LOG_ERR("Error enabling connection ID: %d", errno);
	}

	int timeout = TLS_DTLS_HANDSHAKE_TIMEO_31S;

	LOG_INF("  Setting handshake timeout:");
	err = setsockopt(sock, SOL_TLS, TLS_DTLS_HANDSHAKE_TIMEO, &timeout, sizeof(timeout));
	if (err) {
		LOG_ERR("Error setting handshake timeout: %d", errno);
	}


#elif defined(CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID)
	uint8_t dummy;

	LOG_INF("  Enable connection id:");
	err = setsockopt(sock, SOL_TLS, TLS_DTLS_CONNECTION_ID, &dummy, 0);
	if (err) {
		LOG_ERR("Error enabling connection ID: %d", errno);
	}
#endif

	enum {
		NONE = 0,
		OPTIONAL = 1,
		REQUIRED = 2,
	};

	int verify = OPTIONAL;

	LOG_INF("  Peer verify: %d", verify);
	err = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (err) {
		LOG_ERR("Failed to setup peer verification, errno %d", errno);
		return -errno;
	}

	return err;
}

int dtls_save_session(int sock)
{
#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_CID)
	return -ENOTSUP;
#endif
}

int dtls_load_session(int sock)
{
#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_CID)
	return -ENOTSUP;
#endif
}

int dtls_print_connection_id(int sock, bool verbose)
{
	int err = 0;
#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_CID)
	int status = 0;
	int len = sizeof(status);

	err = getsockopt(sock, SOL_TLS, TLS_DTLS_HANDSHAKE_STATUS, &status, &len);
	if (!err) {
		if (len > 0) {
			if (status == TLS_DTLS_HANDSHAKE_STATUS_FULL) {
				LOG_INF("Full DTLS handshake performed");
			} else if (status == TLS_DTLS_HANDSHAKE_STATUS_CACHED) {
				LOG_INF("Cached DTLS handshake performed");
			} else {
				LOG_WRN("Unknown DTLS handshake status: %d", status);
			}
		} else {
			LOG_WRN("No DTLS status provided");
		}
	} else {
		LOG_ERR("Error retrieving handshake status: %d", errno);
	}

	len = sizeof(status);
	err = getsockopt(sock, SOL_TLS, TLS_DTLS_CID_STATUS, &status, &len);
	if (!err) {
		if (len > 0) {
			switch (status) {
			case TLS_DTLS_CID_STATUS_DISABLED:
				LOG_INF("No DTLS CID used");
				break;
			case TLS_DTLS_CID_STATUS_DOWNLINK:
				LOG_INF("DTLS CID downlink");
				break;
			case TLS_DTLS_CID_STATUS_UPLINK:
				LOG_INF("DTLS CID uplink");
				break;
			case TLS_DTLS_CID_STATUS_BIDIRECTIONAL:
				LOG_INF("DTLS CID bidirectional");
				break;
			default:
				LOG_WRN("Unknown DTLS CID status: %d", status);
				break;
			}
		} else {
			LOG_WRN("No DTLS CID status provided");
		}
	} else {
		LOG_ERR("Error retrieving DTLS CID status: %d", errno);
	}

	len = sizeof(status);
	err = getsockopt(sock, SOL_TLS, TLS_DTLS_CID, &status, &len);
	if (!err) {
		if (len > 0) {
			LOG_INF("DTLS CID: %d", status);
		} else {
			LOG_WRN("No DTLS CID provided");
		}
	} else {
		LOG_ERR("Error retrieving DTLS CID: %d", errno);
	}

#elif defined(CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID)
	static struct tls_dtls_peer_cid last_cid;
	struct tls_dtls_peer_cid cid;
	int cid_len = 0;
	int i;

	cid_len = sizeof(cid);
	memset(&cid, 0, cid_len);
	err = getsockopt(sock, SOL_TLS, TLS_DTLS_PEER_CONNECTION_ID, &cid, &cid_len);
	if (!err) {
		if (last_cid.enabled != cid.enabled) {
			LOG_WRN("CID ENABLE CHANGED");
		}
		if (last_cid.peer_cid_len != cid.peer_cid_len) {
			LOG_WRN("CID LEN CHANGED from %d to %d", last_cid.peer_cid_len,
			       cid.peer_cid_len);
			if (cid.peer_cid_len) {
				LOG_INF("DTLS CID IS  enabled:%d, len:%d ", cid.enabled,
				       cid.peer_cid_len);
				for (i = 0; i < cid.peer_cid_len; i++) {
					LOG_DBG("0x%02x ", cid.peer_cid[i]);
				}
				LOG_INF("");
			}
		} else {
			if (memcmp(last_cid.peer_cid, cid.peer_cid, cid.peer_cid_len) != 0) {
				LOG_WRN("CID CHANGED!");

				LOG_INF("DTLS CID WAS enabled:%d, len:%d ", last_cid.enabled,
				       last_cid.peer_cid_len);
				for (i = 0; i < last_cid.peer_cid_len; i++) {
					LOG_DBG("0x%02x ", last_cid.peer_cid[i]);
				}
				LOG_INF("");
			}
			if (verbose) {
				LOG_INF("DTLS CID IS  enabled:%d, len:%d ", cid.enabled,
				       cid.peer_cid_len);
				for (i = 0; i < cid.peer_cid_len; i++) {
					LOG_DBG("0x%02x ", cid.peer_cid[i]);
				}
				LOG_INF("");
			}
		}
		memcpy(&last_cid, &cid, sizeof(last_cid));
	} else {
		LOG_ERR("Unable to get connection ID: %d", -errno);
	}
#else
	LOG_DBG("Skipping CID");
#endif
	return err;
}
