#ifndef APP_OTA_H
#define APP_OTA_H

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "errno.h"
#include "wifista.h"
#include "esp_http_client.h"

#define BUFFSIZE 1024
#define HASH_LEN 32
#define CONFIG_EXAMPLE_OTA_RECV_TIMEOUT 8000
#define OTA_UPDATE_HTTP_URL "http://bin.bemfa.com/b/347233/3BcYjY3YjFkMDA1MjY2NDAxNjgyMDhhMGNmMGJiYzc3YjM=OTA.bin"

void ota_start(void);
void ota_task(void *pvParameter);

#endif
