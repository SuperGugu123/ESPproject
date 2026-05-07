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

#define DEFAULT_SSID "SuperGu"
#define DEFAULT_PWD "qwer123456"

#define WIFI_CONNECTED_BIT BIT0

void wifista_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);
esp_err_t wifista_init(void);

#endif
