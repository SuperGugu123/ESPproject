#ifndef APP_OTA_H
#define APP_OTA_H

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"    // OTA 操作相关 API
#include "esp_app_format.h" // 固件格式定义
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h" // 非易失性存储
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "protocol_examples_common.h"
#include "errno.h"
#include "wifista.h"

#endif
