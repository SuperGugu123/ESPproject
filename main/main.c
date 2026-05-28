#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "wificsi.h"
#include "wifista.h"
#include "radar.h"
#include "sg90.h"
#include "inmp441.h"
#include "app_sr.h"
#include "app_mqtt.h"
#include "app_ota.h"
#include "lcd.h"
#include "mylvgl.h"

TaskHandle_t radar_task_handle = NULL;

static void radar_task(void *pvParameters)
{
    int last_state = -1;

    while (1)
    {
        if (mqtt_is_connected())
        {
            int current_state;

            if (radar_get_state() == RADAR_STATE_MOTION ||
                radar_get_state() == RADAR_STATE_PRESENCE)
            {
                current_state = 1;
            }
            else
            {
                current_state = 0;
            }

            if (current_state != last_state)
            {
                last_state = current_state;

                if (current_state == 1)
                {
                    mqtt_radar_state_use();
                }
                else
                {
                    mqtt_radar_state_empty();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    /*
    sg90_init();

    ESP_ERROR_CHECK(wifista_init());

    mqtt_app_start();

    ESP_ERROR_CHECK(radar_init());

    ESP_ERROR_CHECK(wifi_ping_router_start());

    BaseType_t task_ret = xTaskCreate(radar_task, "radar_task", 2048, NULL, 5, &radar_task_handle);

    if (task_ret != pdPASS)
    {
        ESP_LOGE("main", "failed to create radar_task");
        return;
    }

    if (RADAR_FORCE_TRAIN_ON_BOOT)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_ERROR_CHECK(radar_train(5000));
    }

    ESP_ERROR_CHECK(inmp441_init());

    ESP_ERROR_CHECK(app_sr_start());

    const esp_app_desc_t *desc = esp_app_get_description();
    printf("version:%s", desc->version);

    */

    // 1. LCD 硬件初始化（屏幕点亮）
    ESP_ERROR_CHECK(lcd_init());

    // 2. LVGL 初始化 + 显示 UI
    lvgl_start(lcd_panel_handle(), lcd_io_handle());
}
