/*
 * Copyright (c) 2020 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/printk.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_event.h>

#include <net/net_if.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>

#include <mbedtls/ssl_ciphersuites.h>

#include <net/nrf_cloud.h>

#include <cJSON.h>
#include <cJSON_os.h>

#include <logging/log.h>

#include "libc_support.h"

LOG_MODULE_REGISTER(esp32_wifi_sta, CONFIG_NRF_CLOUD_WIFI_SAMPLE_LOG_LEVEL);

#define SEC_TAG CONFIG_NRF_CLOUD_SEC_TAG

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(cloud_connected, 0, 1);
static K_SEM_DEFINE(cloud_ready, 0, 1);

static struct net_mgmt_event_callback dhcp_cb;

/**
 * @brief Handle network management events.
 *
 * @param cb - Original handler that called us
 * @param mgmt_event - The network event being notified.
 * @param iface - The network interface generating the event.
 */
static void handler_cb(struct net_mgmt_event_callback *cb,
		    uint32_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
		LOG_WRN("Received mgmt_event:%u", mgmt_event);
		return;
	}

	char buf[NET_IPV4_ADDR_LEN];

	LOG_INF("Your address: %s",
		log_strdup(net_addr_ntop(AF_INET,
				   &iface->config.dhcpv4.requested_ip,
				   buf, sizeof(buf))));
	LOG_INF("Lease time: %u seconds",
			iface->config.dhcpv4.lease_time);
	LOG_INF("Subnet: %s",
		log_strdup(net_addr_ntop(AF_INET,
					&iface->config.ip.ipv4->netmask,
					buf, sizeof(buf))));
	LOG_INF("Router: %s",
		log_strdup(net_addr_ntop(AF_INET,
						&iface->config.ip.ipv4->gw,
						buf, sizeof(buf))));
	k_sem_give(&wifi_connected);
}

/**
 * @brief Handle nrfcloud events.
 *
 * @param evt - the event that occurred.
 */
static void cloud_handler(const struct nrf_cloud_evt *evt)
{
	switch (evt->type) {
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTING:
		LOG_INF("NRF_CLOUD_EVT_TRANSPORT_CONNECTING: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTED:
		LOG_INF("NRF_CLOUD_EVT_TRANSPORT_CONNECTED: %d", evt->status);
		k_sem_give(&cloud_connected);
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST:
		LOG_INF("NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATED:
		LOG_INF("NRF_CLOUD_EVT_USER_ASSOCIATED: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_READY:
		/* we can now send data to cloud */
		LOG_INF("NRF_CLOUD_EVT_READY: %d", evt->status);
		k_sem_give(&cloud_ready);
		break;
	case NRF_CLOUD_EVT_RX_DATA:
		LOG_INF("NRF_CLOUD_EVT_RX_DATA: %d", evt->status);
		LOG_INF("  Data received on topic len: %d: %s",
			evt->topic.len, log_strdup(evt->topic.ptr));
		LOG_INF("  Data len: %d: %s",
			evt->data.len, log_strdup(evt->data.ptr));
		break;
	case NRF_CLOUD_EVT_SENSOR_DATA_ACK:
		LOG_INF("NRF_CLOUD_EVT_SENSOR_DATA_ACK: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
		LOG_INF("NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_FOTA_DONE:
		LOG_INF("NRF_CLOUD_EVT_FOTA_DONE: %d", evt->status);
		break;
	case NRF_CLOUD_EVT_ERROR:
		LOG_ERR("NRF_CLOUD_EVT_ERROR: %d", evt->status);
		break;
	}
}

/**
 * define static arrays containing our credentials; in the future, these
 * should be stored in secure storage
 */
#if defined(MBEDTLS_X509_CRT_PARSE_C)

static const unsigned char ca_certificate[] = {
	#include "ca_cert.h"
};

static const unsigned char client_certificate[] = {
	#include "client_cert.h"
};

static const unsigned char private_key[] = {
	#include "private_key.h"
};

#endif

/**
 * @brief load CA, client cert, and private key to specified security tag
 * in the TLS stack.
 *
 * @param sec_tag the tag to load credentials into.
 * @return 0 on success or negative error code on failure.
 */
static int tls_load_credentials(int sec_tag)
{
	int ret;

	/* Load CA certificate */
	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE,
				ca_certificate, sizeof(ca_certificate));
	if (ret != 0) {
		LOG_ERR("Failed to register CA certificate: %d", ret);
		goto exit;
	}

	/* Load server/client certificate */
	ret = tls_credential_add(sec_tag,
				TLS_CREDENTIAL_SERVER_CERTIFICATE,
				client_certificate, sizeof(client_certificate));
	if (ret < 0) {
		LOG_ERR("Failed to register public cert: %d", ret);
		goto exit;
	}

	/* Load private key */
	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_PRIVATE_KEY,
				private_key, sizeof(private_key));
	if (ret < 0) {
		LOG_ERR("Failed to register private key: %d", ret);
		goto exit;
	}

exit:
	return ret;
}

/**
 * @brief Free up resources from specified security tag.
 *
 * @param sec_tag - the tag to free.
 * @return 0 on success.
 */
static int tls_unloadcrdl(int sec_tag)
{
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE);
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_SERVER_CERTIFICATE);
	tls_credential_delete(sec_tag, TLS_CREDENTIAL_PRIVATE_KEY);

	return 0;
}

/**
 * @brief Update device shadow to indicate the services this device supports.
 *
 * @return 0 on success or negative error code if failed.
 */
static int send_service_info(void)
{
	struct nrf_cloud_svc_info_fota fota_info = {
		.application = false,
		.bootloader = false,
		.modem = false
	};
	struct nrf_cloud_svc_info_ui ui_info = {
		.gps = false,
		.humidity = true,
		.rsrp = false,
		.temperature = true,
		.button = false
	};
	struct nrf_cloud_svc_info service_info = {
		.fota = &fota_info,
		.ui = &ui_info
	};
	struct nrf_cloud_device_status device_status = {
		.modem = NULL,
		.svc = &service_info

	};

	return nrf_cloud_shadow_device_status_update(&device_status);
}

void main(void)
{
	struct net_if *iface;
	int err;
	esp_err_t ret;

	LOG_INF("nRF Cloud WiFi demo starting up...");

	err = tls_load_credentials(SEC_TAG);
	if (err) {
		LOG_ERR("Unable to load credentials: %d", err);
	}

	net_mgmt_init_event_callback(&dhcp_cb, handler_cb,
				     NET_EVENT_IPV4_DHCP_BOUND);

	net_mgmt_add_event_callback(&dhcp_cb);

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("wifi interface not available");
		return;
	}

	net_dhcpv4_start(iface);

	if (!IS_ENABLED(CONFIG_ESP32_WIFI_STA_AUTO)) {
		wifi_config_t wifi_config = {
			.sta = {
				.ssid = CONFIG_ESP32_WIFI_SSID,
				.password = CONFIG_ESP32_WIFI_PASSWORD,
			},
		};

		ret = esp_wifi_set_mode(WIFI_MODE_STA);

		ret |= esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
		ret |= esp_wifi_connect();
		if (ret != ESP_OK) {
			LOG_ERR("Connection failed: %d", ret);
		}
	}
	LOG_INF("Waiting for WiFi connection...");
	k_sem_take(&wifi_connected, K_FOREVER);

	uint8_t mac[6];

	ret |= esp_wifi_get_mac(WIFI_IF_STA, mac);
	if (ret != ESP_OK) {
		LOG_ERR("Could not get WiFi MAC: %d", ret);
	} else {
		LOG_INF("WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

#if defined(TEST_LIBC)
	test_libc();
#endif

	/* connect to nRF Cloud */
	struct nrf_cloud_init_param init_param = {
		.event_handler = cloud_handler,
		.client_id = NULL
	};

	LOG_INF("Initializing nRF Cloud...");
	err = nrf_cloud_init(&init_param);
	if (err) {
		LOG_ERR("Error initializing nRF Cloud: %d", err);
		return;
	}

	/* look up our configured device id */
	char client_id[NRF_CLOUD_CLIENT_ID_MAX_LEN];

	err = nrf_cloud_client_id_get(client_id, sizeof(client_id));
	if (err) {
		LOG_ERR("Error getting client id: %d", err);
	} else {
		LOG_INF("Client id: %s", log_strdup(client_id));
	}

	LOG_INF("Connecting to nRF Cloud...");
	err = nrf_cloud_connect(NULL);
	if (err) {
		LOG_ERR("Error connecting to nRF Cloud: %d", err);
		return;
	}

	LOG_INF("Waiting for Cloud connection to be ready...");
	k_sem_take(&cloud_ready, K_FOREVER);

	char tenant_id[NRF_CLOUD_TENANT_ID_MAX_LEN];

	err = nrf_cloud_tenant_id_get(tenant_id, sizeof(tenant_id));
	if (err) {
		LOG_ERR("Error getting tenant id: %d", err);
	} else {
		LOG_INF("Tenant id: %s", log_strdup(tenant_id));
	}

	err = send_service_info();
	if (err) {
		LOG_ERR("Error sending service info: %d", err);
	} else {
		LOG_INF("Service info sent.");
	}

	/* printf formatter for JSON message to send containing fake temperature data */
	const char data_fmt[] = "{\"appId\":\"TEMP\", \"messageType\":\"DATA\","
				" \"data\":\"%d.%d\"}";
	char data[100];
	struct nrf_cloud_tx_data msg = {
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.data.ptr = data
	};
	float temp = 10.0;

	do
	{
		/* on ESP32, we have to use the Zephyr minimal libc instead of
		 * newlib due to a known incompatibility, so we don't get
		 * floating point printf(); fake it for now
		 */
		snprintf(data, sizeof(data), data_fmt, (int)temp,
			 ((int)(temp * 10)) % 10);
		temp += (rand() - RAND_MAX / 2) * 0.5f / (RAND_MAX / 2);
		msg.data.len = strlen(data);

		LOG_INF("Sending %s to nRF Cloud...", log_strdup(data));

		err = nrf_cloud_send(&msg);
		if (err) {
			LOG_ERR("Error sending message to cloud: %d", err);
			k_sleep(K_SECONDS(1));

#if 0
			k_sem_reset(&wifi_connected);
			LOG_INF("Reconnecting to WiFi...");
			ret = esp_wifi_connect();
			if (ret != ESP_OK) {
				LOG_ERR("Connection failed: %d", ret);
				break;
			}
			k_sem_take(&wifi_connected, K_FOREVER);
			LOG_INF("Connected.");
#endif

			k_sem_reset(&cloud_ready);
			LOG_INF("Reconnecting to nRF Cloud...");
			err = nrf_cloud_connect(NULL);
			if (err) {
				LOG_ERR("Connection failed: %d", err);
				break;
			}
			k_sem_take(&cloud_ready, K_FOREVER);
			LOG_INF("Connected.");
		} else {
			LOG_INF("message sent!");
		}

		k_sleep(K_SECONDS(5));
	} while (1);

	err = nrf_cloud_disconnect();
	if (err) {
		LOG_ERR("Error disconnecting from nRF Cloud: %d", err);
	} else {
		LOG_INF("Disconnected.");
	}

	tls_unloadcrdl(SEC_TAG);
}

