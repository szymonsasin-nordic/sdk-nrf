.. _nrf_coap_client_sample:

nRF9160: nRF CoAP Client
########################

.. contents::
   :local:
   :depth: 2

The nRF CoAP Client sample demonstrates the communication between a public CoAP server and an nRF9160 SiP that acts as the CoAP client.

Requirements
************

The sample supports the following development kit:

.. table-from-sample-yaml::

The sample also requires a public CoAP server IP address or URL available on the Internet.

.. include:: /includes/spm.txt

Overview
********

The nRF CoAP Client sample performs the following actions:

#. Connect to the configured public CoAP test server (specified by the Kconfig option :ref:`CONFIG_COAP_SERVER_HOSTNAME <CONFIG_COAP_SERVER_HOSTNAME>`).
#. Send periodic GET request for a test resource (specified by the Kconfig option :ref:`CONFIG_COAP_RESOURCE <CONFIG_COAP_RESOURCE>`) that is available on the server.
#. Display the received data about the resource on a terminal emulator.

The public CoAP server used in this sample is Californium CoAP server (``coap://californium.eclipseprojects.io:5683``).
This server runs Eclipse Californium, which is an open source implementation of the CoAP protocol that is targeted at the development and testing of IoT applications.
An nRF9160 DK is used as the CoAP client.

This sample uses the resource **obs** (Californium observable resource) in the communication between the CoAP client and the public CoAP server when it issues a CoAP GET request.
The communication follows the standard request/response pattern and is based on the change in the state of the value of the resource.
The sample queries or updates one resource at a time.
It alternates sending a CoAP GET and a CoAP PUT request to the server.
Other resources can be configured using the Kconfig option ``CONFIG_COAP_GET_RESOURCE``.

This sample uses the resource configured using the Kconfig option ``CONFIG_COAP_PUT_RESOURCE`` for the CoAP PUT request.

Configuration
*************

|config|

Configuration options
=====================

Check and configure the following Kconfig options in the :file:`coap_client/prj.conf` file:

.. _CONFIG_COAP_RESOURCE:

CONFIG_COAP_RESOURCE - CoAP resource configuration
   This option sets the CoAP resource. Default is Californium observable resource.

.. _CONFIG_COAP_SERVER_HOSTNAME:

CONFIG_COAP_SERVER_HOSTNAME - CoAP server hostname
   This option sets the CoAP server hostname. Default is ``californium.eclipseprojects.io``.

.. _CONFIG_COAP_SERVER_PORT:

CONFIG_COAP_SERVER_PORT - CoAP server port
   This option sets the port for the CoAP server. Default is ``5683``.

Optional Configurations
***********************

By default, this sample establishes an unencrypted UDP connection to the CoAP server on port 5683.

Alternatively, it can be configured to use DTLS encryption with PSK (preshared keys) on port 5684.
Note that the default CoAP test server does not support DTLS.
Configure this sample option to enable DTLS:

* :kconfig:`CONFIG_COAP_DTLS`

There are two choices for DTLS encryption mechanisms.
When you enable DTLS, you must apply the associated config overlay for one of them:

#. Offloaded to the nRF9160 modem's UDP and DTLS stacks.
#. UDP offloaded to the nRF9160 modem, and DTLS implemented on the application side using mbedTLS.

The advantage of using mbedTLS is that it provides DTLS Connection ID support, which helps eliminate unnecessary network traffic.

The following files are available:

* :file:`overlay-mbedtls.conf` - Config overlay to use mbedTLS for DTLS.
* :file:`overlay-offload-tls.conf` - Config overlay to use the nRF9160 modem for DTLS.
* :file:`overlay-carrier.conf` - Config overlay for LWM2M carrier support.
* :file:`overlay-debug.conf` - Config overlay for logging support.

Building and running
********************

To build this sample using an overlay, use the ``-DOVERLAY_CONFIG=overlay-file.conf`` option.
For example:

``west build -p -b nrf9160dk_nrf9160_ns -- -DOVERLAY_CONFIG=overlay-mbedtls.conf``

.. |sample path| replace:: :file:`samples/nrf9160/coap_client`

.. include:: /includes/build_and_run_nrf9160.txt


Testing
=======

|test_sample|

1. |connect_kit|
#. |connect_terminal|
#. Power on or reset the kit.
#. Observe that the following output is displayed in the terminal::

       The nRF CoAP client sample started
#. Observe that the discovered IP address of the public CoAP server is displayed on the terminal emulator.
#. Observe that the nRF9160 DK sends periodic CoAP GET requests to the configured server for a configured resource after it gets LTE connection.
#. Observe that the sample either displays the response data received from the server or indicates a timeout on the terminal.
   For more information on the response codes, see `COAP response codes`_.



Sample output
=============

The sample displays the data in the following format:

.. code-block:: console

   CoAP request sent: token 0x9772
   CoAP response: code: 0x45, token 0x9772, payload: 15:39:40

Instead of displaying every single CoAP frame content, the sample displays only the essential data.
For the above sample output, the information displayed on the terminal conveys the following:

* ``code:0x45`` -  CoAP response code (2.05 - Content), which is constant across responses
* ``token 0x9772`` - CoAP token, which is unique per request/response pair
* ``payload: 15:39:40`` - the actual message payload (current time in UTC format) from the resource that is queried in this sample

References
**********

`RFC 7252 - The Constrained Application Protocol`_
`RFC 9146 - Connection Identifier for DTLS 1.2`_

Dependencies
************

This sample uses the following |NCS| libraries:

* :ref:`lte_lc_readme`

It uses the following `sdk-nrfxlib`_ library:

* :ref:`nrfxlib:nrf_modem`

It uses the following Zephyr library:

* :ref:`CoAP <zephyr:networking_api>`

In addition, it uses the following sample:

* :ref:`secure_partition_manager`
