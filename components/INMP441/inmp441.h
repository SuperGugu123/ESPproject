#ifndef __INMP441_H_
#define __INMP441_H_

#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"

#define INMP441_SD_GPIO GPIO_NUM_15
#define INMP441_SCK_GPIO GPIO_NUM_16
#define INMP441_WS_GPIO GPIO_NUM_17

#define INMP441_SAMPLE_RATE_HZ 16000
#define INMP441_DMA_DESC_NUM 4
#define INMP441_DMA_FRAME_NUM 256

esp_err_t inmp441_init(void);
esp_err_t inmp441_read(int32_t *samples,
                       size_t sample_count,
                       size_t *samples_read,
                       TickType_t timeout_ticks);

#endif
