.. _nrf_cloud_wifi:

Espressif ESP32 nRF Cloud WiFi Sample
#####################################

Overview
********

This sample demonstrates how to use ESP32 to connect to a WiFi network as a station device.
To configure WiFi credentials, edit ``prj.conf``.
Enabling the ``net_shell`` module provides a set of commands to test the connection.
See :ref:`network shell <net_shell>` for more details.

Building and Running
********************

Make sure you have the ESP32 connected over USB port.

The sample can be built and flashed as follows.

0. (One time only) create self-signed ca certificate with `create_ca_cert.py`

   See: https://github.com/nRFCloud/utils/tree/master/python/modem-firmware-1.3%2B

1.  Create new WiFi device credentials
::
   python3 create_device_credentials.py -ca CA0x..._ca.pem -ca_key CA0x..._prv.pem -c US -st OR -l Portland -o Nordic -ou Cloud -cn $DEVICE_ID -e $EMAIL -dv 2000 -p ./wifi -f "$DEVICE_ID-"

2. Create .csv file with $DEVICE_ID and $DEVICE_ID_crt.pem
::
   $DEVICE_ID,,,,"-----BEGIN CERTIFICATE-----
   (contents of $DEVICE_ID_crt.pem)
   -----END CERTIFICATE-----
   "

3. Send the csv file to nrfcloud ProvisionDevices endpoint

   Visit: https://nrfcloud.com/#/provision-devices and upload your csv file.
   This provisions the device in AWS IoT and adds it to your nRF Cloud account.
   Your device will appear on the devices page: https://nrfcloud.com/#/devices.

4. Convert each to a header byte array
::
   "cat aws_ca.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > ca_cert.h"
   "cat $DEVICE_ID_crt.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > client_cert.h"
   "cat $DEVICE_ID_prv.pem | sed -e '1d;$d' | base64 -d -i |xxd -i > private_key.h"

5. Edit prj.conf to use your new device ID

   `CONFIG_NRF_CLOUD_CLIENT_ID="$DEVICE_ID"`
   
6. Build and install firmware
::
   $ export ESPRESSIF_TOOLCHAIN_PATH=$HOME/.espressif/tools/zephyr/xtensa-esp32-elf
   $ export ZEPHYR_TOOLCHAIN_VARIANT=espressif
   $ west build -p auto -b esp32 -d build_nrfc_wifi samples/wifi/nrf_cloud_wifi
   $ west flash -d build_nrfc_wifi --esp-device <serial device, e.g. /dev/ttyUSB2 or COM42 on Windows)
   
Sample Output
=============

To check output of this sample, any serial console program can be used (i.e. on Linux minicom, putty, screen, etc)
This example uses ``picocom`` on the serial port ``/dev/ttyUSB0``:

.. code-block:: console

   $ picocom /dev/ttyUSB0 - 115200

.. code-block:: console

    I (29) boot: ESP-IDF release/v4.3-49-gc21d252a2-dirt 2nd stage bootloader
    I (29) boot: compile time 15:14:22
    I (30) boot: chip revision: 3
    I (34) boot_comm: chip revision: 3, min. bootloader chip revision: 0
    I (41) boot.esp32: SPI Speed      : 40MHz
    I (46) boot.esp32: SPI Mode       : DIO
    I (50) boot.esp32: SPI Flash Size : 8MB
    I (55) boot: Enabling RNG early entropy source...
    I (60) boot: Partition Table:
    I (64) boot: ## Label            Usage          Type ST Offset   Length
    I (71) boot:  0 nvs              WiFi data        01 02 00002000 00006000
    I (79) boot:  1 phy_init         RF data          01 01 00008000 00001000
    I (86) boot:  2 factory          factory app      00 00 00010000 00100000
    I (94) boot: End of partition table
    I (98) boot_comm: chip revision: 3, min. application chip revision: 0
    I (105) esp_image: segment 0: paddr=00010020 vaddr=3f400020 size=121d0h ( 74192) map
    I (141) esp_image: segment 1: paddr=000221f8 vaddr=3ffb0000 size=00434h (  1076) load
    I (142) esp_image: segment 2: paddr=00022634 vaddr=3ffb0438 size=05a40h ( 23104) load
    I (157) esp_image: segment 3: paddr=0002807c vaddr=3ffb5e78 size=001c4h (   452) load
    I (157) esp_image: segment 4: paddr=00028248 vaddr=40080000 size=00400h (  1024) load
    I (165) esp_image: segment 5: paddr=00028650 vaddr=40080400 size=079c8h ( 31176) load
    I (186) esp_image: segment 6: paddr=00030020 vaddr=400d0020 size=583b4h (361396) map
    I (323) esp_image: segment 7: paddr=000883dc vaddr=40087dc8 size=0feech ( 65260) load
    I (364) boot: Loaded app from partition at offset 0x10000
    I (364) boot: Disabling RNG early entropy source...
    I (770) wifi:wifi driver task: 3ffb61a8, prio:2, stack:3584, core=0
    I (771) wifi:wifi firmware version: 7e2c914
    I (772) wifi:wifi certification version: v7.0
    I (773) wifi:config NVS flash: disabled
    I (777) wifi:config nano formating: disabled
    I (781) wifi:Init data frame dynamic rx buffer num: 32
    I (786) wifi:Init management frame dynamic rx buffer num: 32
    I (791) wifi:Init management short buffer num: 32
    I (795) wifi:Init dynamic tx buffer num: 32
    I (799) wifi:Init static rx buffer size: 1600
    I (804) wifi:Init static rx buffer num: 10
    I (807) wifi:Init dynamic rx buffer num: 32
    I (812) phy_init: phy_version 4670,719f9f6,Feb 18 2021,17:07:07
    I (974) wifi:mode : softAP (7c:87:ce:e3:46:9d)
    I (975) wifi:Total power save buffer number: 16
    I (975) wifi:Init max length of beacon: 752/752
    I (977) wifi:Init max length of beacon: 752/752
    I (982) wifi:mode : sta (7c:87:ce:e3:46:9c)
    I (984) wifi:enable tsf
    I (1597) wifi:new:<6,0>, old:<1,1>, ap:<255,255>, sta:<6,0>, prof:1
    I (2248) wifi:state: init -> auth (b0)
    *** Booting Zephyr OS build v2.6.99-ncs1-13-g20ac60fb9670  ***
    [00:00:00.981,000] <inf> esp32_wifi_sta: nRF Cloud WiFi demo starting I (2253) wifi:state: auth -> assoc (0)
    up...
    [00:00:00.988,000] <inf> esp32_wifi_sta: I (2261) wifi:state: assoc -> run (10)
    Waiting for WiFi connW (2268) wifi:<ba-add>idx:0 (ifx:0, 6c:55:e8:9b:84:65), tid:5, ssn:0, winSize:64
    ection...
    [00:00:00.988,000] <inf> esp32_wifi: WIFI_EVENT_STA_START
    I (2288) wifi:connected with CBCI-8461, aid = 3, channel 6, BW20, bssid = 6c:55:e8:9b:84:65
    I (2288) wifi:security: WPA2-PSK, phy: bgn, rssi: -69
    I (2293) wifi:pm start, type: 1
    
    I (2329) wifi:AP's beacon interval = 102400 us, DTIM period = 1
    W (2755) wifi:<ba-add>idx:1 (ifx:0, 6c:55:e8:9b:84:65), tid:0, ssn:0, winSize:64
    [00:00:02.297,000] <inf> esp32_wifi: WIFI_EVENT_STA_CONNECTED
    [00:00:04.020,000] <inf> net_dhcpv4: Received: 10.1.10.67
    [00:00:04.020,000] <inf> esp32_wifi_sta: Your address: 10.1.10.67
    [00:00:04.020,000] <inf> esp32_wifi_sta: Lease time: 604800 seconds
    [00:00:04.020,000] <inf> esp32_wifi_sta: Subnet: 255.255.255.0
    [00:00:04.020,000] <inf> esp32_wifi_sta: Router: 10.1.10.1
    [00:00:04.020,000] <inf> esp32_wifi_sta: **************
    [00:00:04.020,000] <inf> esp32_wifi_sta: strtod(3.14159) returned: 3.141
    [00:00:04.020,000] <inf> esp32_wifi_sta: sscanf() returned 3; string: apple, int:12, double:0.000
    [00:00:04.021,000] <inf> esp32_wifi_sta: Parsing {"appId": "HUMID", "messageType": "DATA","data": "70.0"}...
    [00:00:04.021,000] <inf> esp32_wifi_sta: Result: {
            "appId":        "HUMID",
            "messageType":  "DATA",
            "data": "70.0"
    }
    [00:00:04.021,000] <inf> esp32_wifi_sta: Data str: "70.0"
    [00:00:04.021,000] <inf> esp32_wifi_sta: deleting object...
    [00:00:04.021,000] <inf> esp32_wifi_sta: **************
    [00:00:04.021,000] <inf> esp32_wifi_sta: Initializing nRF Cloud...
    [00:00:04.021,000] <dbg> nrf_cloud_transport.nct_client_id_set: client_id = wifi-12345
    [00:00:04.022,000] <dbg> nrf_cloud_transport.nct_topics_populate: accepted_topic: wifi-12345/shadow/get/accepted
    [00:00:04.022,000] <dbg> nrf_cloud_transport.nct_topics_populate: rejected_topic: $aws/things/wifi-12345/shadow/get/rejected
    [00:00:04.022,000] <dbg> nrf_cloud_transport.nct_topics_populate: update_delta_topic: $aws/things/wifi-12345/shadow/update/delta
    [00:00:04.022,000] <dbg> nrf_cloud_transport.nct_topics_populate: update_topic: $aws/things/wifi-12345/shadow/update
    [00:00:04.022,000] <dbg> nrf_cloud_transport.nct_topics_populate: shadow_get_topic: $aws/things/wifi-12345/shadow/get
    [00:00:04.022,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 1
    [00:00:04.022,000] <inf> esp32_wifi_sta: Client id: wifi-12345
    [00:00:04.022,000] <inf> esp32_wifi_sta: Connecting to nRF Cloud...
    [00:00:04.022,000] <inf> esp32_wifi_sta: Waiting for Cloud connection to be ready...
    [00:00:04.202,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 1
    [00:00:04.202,000] <inf> esp32_wifi_sta: NRF_CLOUD_EVT_TRANSPORT_CONNECTING: 0
    [00:00:04.202,000] <dbg> net_dns_resolve.dns_write: (0x3ffb6488): [0] submitting work to server idx 0 for id 2791 hash 25421
    [00:00:04.231,000] <dbg> nrf_cloud_transport.nct_connect: IPv4 address: 3.231.244.115
    [00:00:04.231,000] <dbg> nrf_cloud_transport.nct_mqtt_connect: MQTT clean session flag: 1
    [00:00:05.827,000] <dbg> nrf_cloud.nrf_cloud_run: Cloud connection request sent
    [00:00:06.040,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_CONNACK: result 0
    [00:00:06.040,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 2
    [00:00:06.040,000] <inf> esp32_wifi_sta: NRF_CLOUD_EVT_TRANSPORT_CONNECTED: 0
    [00:00:06.040,000] <dbg> nrf_cloud_transport.nct_cc_connect: nct_cc_connect
    [00:00:06.043,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 3
    [00:00:06.227,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_SUBACK: id = 100 result = 0
    [00:00:06.227,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 4
    [00:00:06.227,000] <dbg> nrf_cloud_transport.nct_cc_send: mqtt_publish: id = 200 opcode = 0 len = 0
    [00:00:06.231,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 5
    [00:00:06.313,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBLISH: id = 1 len = 484
    [00:00:06.319,000] <dbg> nrf_cloud_transport.nct_dc_endpoint_set: nct_dc_endpoint_set
    [00:00:06.319,000] <dbg> nrf_cloud_transport.nct_dc_endpoint_get: nct_dc_endpoint_get
    [00:00:06.320,000] <dbg> nrf_cloud_transport.nct_cc_send: mqtt_publish: id = 301 opcode = 1 len = 345
    [00:00:06.326,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 7
    [00:00:06.326,000] <inf> esp32_wifi_sta: NRF_CLOUD_EVT_USER_ASSOCIATED: 0
    [00:00:06.333,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 200 result = 0
    [00:00:06.333,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 5
    [00:00:06.535,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 301 result = 0
    [00:00:06.546,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBLISH: id = 936 len = 265
    [00:00:06.548,000] <dbg> nrf_cloud_transport.nct_dc_endpoint_set: nct_dc_endpoint_set
    [00:00:06.548,000] <dbg> nrf_cloud_transport.nct_dc_endpoint_get: nct_dc_endpoint_get
    [00:00:06.549,000] <dbg> nrf_cloud_transport.nct_cc_send: mqtt_publish: id = 301 opcode = 1 len = 345
    [00:00:06.658,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 7
    [00:00:06.658,000] <inf> esp32_wifi_sta: NRF_CLOUD_EVT_USER_ASSOCIATED: 0
    [00:00:06.841,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 301 result = 0
    [00:00:06.841,000] <dbg> nrf_cloud_transport.nct_dc_connect: nct_dc_connect
    [00:00:06.844,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 8
    [00:00:07.045,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_SUBACK: id = 101 result = 0
    [00:00:07.045,000] <dbg> nrf_cloud_transport.nct_save_session_state: Setting session state: 1
    [00:00:07.045,000] <err> nrf_cloud_transport: Failed to save session state: -2
    [00:00:07.045,000] <dbg> nrf_cloud.nfsm_set_current_state_and_notify: state: 9
    [00:00:07.045,000] <inf> esp32_wifi_sta: NRF_CLOUD_EVT_READY: 0
    [00:00:07.045,000] <inf> esp32_wifi_sta: Tenant id: e07f25b0-a92b-4043-af5e-d556700daeee
    [00:00:07.045,000] <dbg> nrf_cloud_transport.nct_cc_send: mqtt_publish: id = 1000 opcode = 1 len = 88
    [00:00:07.048,000] <inf> esp32_wifi_sta: nRF Cloud service info sent
    [00:00:07.048,000] <inf> esp32_wifi_sta: Sending {"appId":"TEMP", "messageType":"DATA", "data":"10.0"} to nRF Cloud...
    [00:00:07.050,000] <inf> esp32_wifi_sta: message sent!
    [00:00:07.255,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1000 result = 0
    [00:00:07.256,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1001 result = 0
    [00:00:07.268,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBLISH: id = 1 len = 484
    [00:00:12.050,000] <inf> esp32_wifi_sta: Sending {"appId":"TEMP", "messageType":"DATA", "data":"10.4"} to nRF Cloud...
    [00:00:12.053,000] <inf> esp32_wifi_sta: message sent!
    [00:00:12.170,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1002 result = 0
    [00:00:17.053,000] <inf> esp32_wifi_sta: Sending {"appId":"TEMP", "messageType":"DATA", "data":"10.4"} to nRF Cloud...
    [00:00:17.056,000] <inf> esp32_wifi_sta: message sent!
    [00:00:17.370,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1003 result = 0
    [00:00:22.056,000] <inf> esp32_wifi_sta: Sending {"appId":"TEMP", "messageType":"DATA", "data":"10.0"} to nRF Cloud...
    [00:00:22.059,000] <inf> esp32_wifi_sta: message sent!
    [00:00:22.264,000] <dbg> nrf_cloud_transport.nct_mqtt_evt_handler: MQTT_EVT_PUBACK: id = 1004 result = 0
    [00:00:27.059,000] <inf> esp32_wifi_sta: Sending {"appId":"TEMP", "messageType":"DATA", "data":"10.2"} to nRF Cloud...

Sample console interaction
==========================

If the :kconfig:`CONFIG_NET_SHELL` option is set, network shell functions
can be used to check internet connection.

.. code-block:: console

   shell> net ping 8.8.8.8
   PING 8.8.8.8
   28 bytes from 8.8.8.8 to 192.168.68.102: icmp_seq=0 ttl=118 time=19 ms
   28 bytes from 8.8.8.8 to 192.168.68.102: icmp_seq=1 ttl=118 time=16 ms
   28 bytes from 8.8.8.8 to 192.168.68.102: icmp_seq=2 ttl=118 time=21 ms
