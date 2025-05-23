/* Common functions for protocol examples, to establish Wi-Fi or Ethernet connection.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_netif.h"
#if CONFIG_EXAMPLE_CONNECT_ETHERNET
#include "esp_eth.h"
#endif
#endif // !CONFIG_IDF_TARGET_LINUX

#ifdef __cplusplus
extern "C" {
#endif

#if !CONFIG_IDF_TARGET_LINUX
#if CONFIG_EXAMPLE_CONNECT_WIFI
#define EXAMPLE_NETIF_DESC_STA "example_netif_sta"
#endif

#if CONFIG_EXAMPLE_CONNECT_ETHERNET
#define EXAMPLE_NETIF_DESC_ETH "example_netif_eth"
#endif

#if CONFIG_EXAMPLE_CONNECT_THREAD
#define EXAMPLE_NETIF_DESC_THREAD "example_netif_thread"
#endif

#if CONFIG_EXAMPLE_CONNECT_PPP
#define EXAMPLE_NETIF_DESC_PPP "example_netif_ppp"
#endif

#if CONFIG_EXAMPLE_WIFI_SCAN_METHOD_FAST
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_FAST_SCAN
#elif CONFIG_EXAMPLE_WIFI_SCAN_METHOD_ALL_CHANNEL
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#endif

#if CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SIGNAL
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SECURITY
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#endif

#if CONFIG_EXAMPLE_WIFI_AUTH_OPEN
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_EXAMPLE_WIFI_AUTH_WEP
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA2_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA_WPA2_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA2_ENTERPRISE
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_ENTERPRISE
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA3_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_EXAMPLE_WIFI_AUTH_WPA2_WPA3_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_EXAMPLE_WIFI_AUTH_WAPI_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* Example default interface, prefer the ethernet one if running in example-test (CI) configuration */
#if CONFIG_EXAMPLE_CONNECT_ETHERNET
#define EXAMPLE_INTERFACE get_example_netif_from_desc(EXAMPLE_NETIF_DESC_ETH)
#define get_example_netif() get_example_netif_from_desc(EXAMPLE_NETIF_DESC_ETH)
#elif CONFIG_EXAMPLE_CONNECT_WIFI
#define EXAMPLE_INTERFACE get_example_netif_from_desc(EXAMPLE_NETIF_DESC_STA)
#define get_example_netif() get_example_netif_from_desc(EXAMPLE_NETIF_DESC_STA)
#elif CONFIG_EXAMPLE_CONNECT_THREAD
#define EXAMPLE_INTERFACE get_example_netif_from_desc(EXAMPLE_NETIF_DESC_THREAD)
#define get_example_netif() get_example_netif_from_desc(EXAMPLE_NETIF_DESC_THREAD)
#endif

/**
 * @brief Configure Wi-Fi or Ethernet, connect, wait for IP
 *
 * This all-in-one helper function is used in protocols examples to
 * reduce the amount of boilerplate in the example.
 *
 * It is not intended to be used in real world applications.
 * See examples under examples/wifi/getting_started/ and examples/ethernet/
 * for more complete Wi-Fi or Ethernet initialization code.
 *
 * Read "Establishing Wi-Fi or Ethernet Connection" section in
 * examples/protocols/README.md for more information about this function.
 *
 * @return ESP_OK on successful connection
 */
esp_err_t example_connect(void);

/**
 * Counterpart to example_connect, de-initializes Wi-Fi or Ethernet
 */
esp_err_t example_disconnect(void);

/**
 * @brief Configure stdin and stdout to use blocking I/O
 *
 * This helper function is used in ASIO examples. It wraps installing the
 * UART driver and configuring VFS layer to use UART driver for console I/O.
 */
esp_err_t example_configure_stdin_stdout(void);

/**
 * @brief Returns esp-netif pointer created by example_connect() described by
 * the supplied desc field
 *
 * @param desc Textual interface of created network interface, for example "sta"
 * indicate default WiFi station, "eth" default Ethernet interface.
 *
 */
esp_netif_t *get_example_netif_from_desc(const char *desc);

#if CONFIG_EXAMPLE_PROVIDE_WIFI_CONSOLE_CMD
/**
 * @brief Register wifi connect commands
 *
 * Provide a simple wifi_connect command in esp_console.
 * This function can be used after esp_console is initialized.
 */
void example_register_wifi_connect_commands(void);
#endif

#if CONFIG_EXAMPLE_CONNECT_ETHERNET
/**
 * @brief Get the example Ethernet driver handle
 *
 * @return esp_eth_handle_t
 */
esp_eth_handle_t get_example_eth_handle(void);
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

#else
static inline esp_err_t example_connect(void) {return ESP_OK;}
#endif // !CONFIG_IDF_TARGET_LINUX

#ifdef __cplusplus
}
#endif
