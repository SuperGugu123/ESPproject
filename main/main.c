#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wificsi.h"
#include "wifista.h"
#include "radar.h"
#include "sg90.h"
#include "inmp441.h"
#include "app_sr.h"
#include "app_mqtt.h"

TaskHandle_t sg90_task_handle = NULL;
static TaskHandle_t mqtt_sg90_demo_handle = NULL;

static void mqtt_sg90_demo_task(void *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        if (!mqtt_is_connected())
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t ret = mqtt_sg90_on();
        if (ret == ESP_OK)
        {
            ESP_LOGI("main", "Demo publish: sg90 on");
        }
        else
        {
            ESP_LOGW("main", "Skip sg90 on: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(5000));

        if (!mqtt_is_connected())
        {
            continue;
        }

        ret = mqtt_sg90_off();
        if (ret == ESP_OK)
        {
            ESP_LOGI("main", "Demo publish: sg90 off");
        }
        else
        {
            ESP_LOGW("main", "Skip sg90 off: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void sg90_task(void *pvParameters)
{
    while (1)
    {
        if (radar_get_state() == RADAR_STATE_MOTION)
        {
            sg90_set_angle_from_radar(SG90_DEFAULT_OPEN_ANGLE);
        }
        else if (radar_get_state() == RADAR_STATE_PRESENCE)
        {
            sg90_set_angle_from_radar(60);
        }
        else
        {
            sg90_set_angle_from_radar(SG90_MIN_ANGLE);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    sg90_init();

    /* 先只保留 Wi-Fi + MQTT，验证基础通信和下发控制。 */
    ESP_ERROR_CHECK(wifista_init());
    mqtt_app_start();

    BaseType_t task_ret = xTaskCreate(mqtt_sg90_demo_task, "mqtt_sg90_demo", 3072, NULL, 5, &mqtt_sg90_demo_handle);
    if (task_ret != pdPASS)
    {
        ESP_LOGE("main", "failed to create mqtt_sg90_demo_task");
    }

    /*
    ESP_ERROR_CHECK(inmp441_init());

    ESP_ERROR_CHECK(radar_init());

    ESP_ERROR_CHECK(wifi_ping_router_start());

    if (RADAR_FORCE_TRAIN_ON_BOOT)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_ERROR_CHECK(radar_train(5000));
    }

    BaseType_t task_ret = xTaskCreate(sg90_task, "sg90_task", 2048, NULL, 5, &sg90_task_handle);

    if (task_ret != pdPASS)
    {
        ESP_LOGE("main", "failed to create sg90_task");
        return;
    }

    ESP_ERROR_CHECK(app_sr_start());
    */

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
