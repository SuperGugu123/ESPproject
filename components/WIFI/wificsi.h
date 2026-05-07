#ifndef __WIFICSI_H_
#define __WIFICSI_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "esp_event.h"
#include "wifista.h"

#define CONFIG_SEND_FREQUENCY 100

void wifi_csi_init(void);
esp_err_t wifi_ping_router_start(void);
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info);

#endif
