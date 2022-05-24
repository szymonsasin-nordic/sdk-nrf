/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
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
#if defined(CONFIG_MODEM_INFO)
#include <modem/modem_info.h>
#endif
#if defined(CONFIG_MBEDTLS)
#include <mbedtls/ssl.h>
#include <mbedtls/debug.h>
#endif
#if defined(CONFIG_TLS_CREDENTIALS)
#include <net/tls_credentials.h>
#endif
#include <nrf_socket.h>
#include <nrf_modem_at.h>

#include <logging/log.h>
LOG_MODULE_DECLARE(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#if defined(CONFIG_MBEDTLS)
/* Uncomment to limit cipher negotation to a list */
#define RESTRICT_CIPHERS
/* Uncomment to display the cipherlist available */
/* #define DUMP_CIPHERLIST */
#endif

static sec_tag_t sectag = CONFIG_COAP_SECTAG;

#if defined(CONFIG_MODEM_INFO)
static struct modem_param_info mdm_param;

static int get_device_ip_address(uint8_t *d4_addr)
{
	int err;

	err = modem_info_init();
	if (err) {
		printk("Could not init modem info: %d\n", err);
		return err;
	}
	err = modem_info_params_init(&mdm_param);
	if (err) {
		printk("Could not get modem info: %d\n", err);
		return err;
	}

	err = modem_info_params_get(&mdm_param);
	if (err) {
		printk("Could not get modem params: %d\n", err);
		return err;
	}
	LOG_DBG("nRF Connect SDK version: %s", mdm_param.device.app_version);
	LOG_DBG("Modem FW version:        %s", mdm_param.device.modem_fw.value_string);
	LOG_DBG("IMEI:                    %s", mdm_param.device.imei.value_string);

	err = inet_pton(AF_INET, mdm_param.network.ip_address.value_string, d4_addr);
	if (err == 1) {
		return 0;
	}
	return errno;
}
#endif /* CONFIG_MODEM_INFO */

static int provision_psk(void)
{
	int ret;
	const char *identity_fmt = "n%s0123456789abcdef";
	static char identity[120];
	uint16_t identity_len;
	const char *psk = "000102030405060708090a0b0c0d0e0f";
	uint16_t psk_len;

	snprintf(identity, sizeof(identity), identity_fmt,  mdm_param.device.imei.value_string);
	identity_len = strlen(identity);

	LOG_DBG("psk identity: %s len %u", log_strdup(identity), identity_len);

	psk_len = strlen(psk);
	LOG_HEXDUMP_DBG(psk, psk_len, "psk");

#if defined(CONFIG_NET_SOCKETS_OFFLOAD_TLS)
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

int client_dtls_init(int sock)
{
	int err;
	uint8_t d4_addr[4];
#if defined(RESTRICT_CIPHERS)
	static const int ciphers[] = {
		MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8, 0
	};
#endif

	err = get_device_ip_address(d4_addr);
	if (err) {
		return err;
	}
	printk("client addr %u.%u.%u.%u\n", d4_addr[0], d4_addr[1], d4_addr[2], d4_addr[3]);

	printk("Provisioning PSK to sectag %u\n", sectag);
	err = provision_psk();
	if (err) {
		return err;
	}
	printk("Setting socket options\n");

	err = setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_COAP_SERVER_HOSTNAME,
			 sizeof(CONFIG_COAP_SERVER_HOSTNAME));
	if (err) {
		printk("Error setting hostname: %d\n", errno);
	}

	err = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, &sectag, sizeof(sec_tag_t));
	if (err) {
		printk("Error setting sectag list: %d\n", errno);
	}

#if defined(RESTRICT_CIPHERS)
	err = setsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, ciphers, sizeof(ciphers));
	if (err) {
		printk("Error setting cipherlist: %d\n", errno);
	}
#endif
#if defined(DUMP_CIPHERLIST)
	int len;
	int ciphers[32];

	len = sizeof(ciphers);
	err = getsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_LIST, ciphers, &len);
	if (err) {
		printk("Error getting cipherlist: %d\n", errno);
	} else {
		int count = len / sizeof(int);

		printk("New cipherlist:\n");
		for (int i = 0; i < count; i++) {
			printk("%d. 0x%04X = %s\n", i, (unsigned int)ciphers[i],
			       mbedtls_ssl_get_ciphersuite_name(ciphers[i]));
		}
	}
#endif

#if defined(CONFIG_MBEDTLS_SSL_DTLS_CONNECTION_ID)
	uint8_t dummy;

	err = setsockopt(sock, SOL_TLS, TLS_DTLS_CONNECTION_ID, &dummy, 0);
	if (err) {
		printk("Error setting connection ID: %d\n", errno);
	}
#endif
	return err;
}

int client_print_connection_id(int sock, bool verbose)
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
			if (verbose) {
				printk("DTLS CID IS  enabled:%d, len:%d ", cid.enabled,
				       cid.peer_cid_len);
				for (i = 0; i < cid.peer_cid_len; i++) {
					printk("0x%02x ", cid.peer_cid[i]);
				}
				printk("\n");
			}
		}
		memcpy(&last_cid, &cid, sizeof(last_cid));
	} else {
		LOG_ERR("Unable to get connection ID: %d", -errno);
	}
#else
	LOG_INF("Skipping CID");
#endif
	return err;
}
