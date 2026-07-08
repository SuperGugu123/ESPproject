#ifndef __WIFISTA_H_
#define __WIFISTA_H_

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "nvs_flash.h"

// ========== NVS 凭证存取 ==========
#define WIFI_NVS_NS "wifi_cfg"
#define WIFI_NVS_SSID "ssid"
#define WIFI_NVS_PWD "pwd"

#define WIFI_CONNECTED_BIT BIT0

void wifista_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);
esp_err_t wifista_base_init(void);
esp_err_t wifi_connect_to_ap(const char *ssid, const char *password);
bool wifi_is_connected(void);
esp_err_t wifi_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len, char *pwd, size_t pwd_len);

#endif
