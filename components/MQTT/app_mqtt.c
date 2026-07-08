#include "app_mqtt.h"
#include "app_ota.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sg90.h"

static const char *TAG = "APP_MQTT";
static const char *MQTT_SG90_CMD_TOPIC = "esp32/sg90/cmd";
static const char *MQTT_OTA_CMD_TOPIC = "esp32/ota/cmd";

static bool mqtt_connected = false;
static esp_mqtt_client_handle_t client = NULL;

esp_err_t mqtt_publish_command(const char *topic, const char *payload)
{
    ESP_RETURN_ON_FALSE(mqtt_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_STATE, TAG, "MQTT client is not initialized");

    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "Publish failed, topic=%s, msg_id=%d", topic, msg_id);

    ESP_LOGI(TAG, "Publish queued, topic=%s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_sg90_on(void)
{
    return mqtt_publish_command(
        "esp32/control",
        "{\"input\": \"sg90 on\", \"siteId\": \"esp32\"}");
}

esp_err_t mqtt_sg90_off(void)
{
    return mqtt_publish_command(
        "esp32/control",
        "{\"input\": \"sg90 off\", \"siteId\": \"esp32\"}");
}

esp_err_t mqtt_radar_state_empty(void)
{
    return mqtt_publish_command(
        "esp32/radar/state",
        "empty");
}

esp_err_t mqtt_radar_state_use(void)
{
    return mqtt_publish_command(
        "esp32/radar/state",
        "use");
}

bool mqtt_is_connected(void)
{
    return mqtt_connected;
}

void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "%s: 0x%x", message, error_code);
    }
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;

    ESP_LOGD(TAG, "Event dispatched, base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        {
            int msg_id = esp_mqtt_client_subscribe(client, MQTT_SG90_CMD_TOPIC, 1);
            int msg_id_ota = esp_mqtt_client_subscribe(client, MQTT_OTA_CMD_TOPIC, 1);
            if (msg_id > 0)
            {
                ESP_LOGI(TAG, "Subscribe queued, topic=%s, msg_id=%d", MQTT_SG90_CMD_TOPIC, msg_id);
            }
            if (msg_id_ota > 0)
            {
                ESP_LOGI(TAG, "Subscribe queued, topic=%s, msg_id=%d", MQTT_OTA_CMD_TOPIC, msg_id_ota);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:

        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:

        ESP_LOGI(TAG, "MQTT data received");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

        if (event->topic_len == strlen(MQTT_SG90_CMD_TOPIC) &&
            strncmp(event->topic, MQTT_SG90_CMD_TOPIC, event->topic_len) == 0)
        {

            if (event->current_data_offset != 0 || event->data_len != event->total_data_len)
            {
                ESP_LOGW(TAG, "Ignore fragmented MQTT payload");
                break;
            }

            char payload[64] = {0};
            int copy_len = event->data_len;
            if (copy_len >= (int)sizeof(payload))
            {
                copy_len = sizeof(payload) - 1;
            }
            memcpy(payload, event->data, copy_len);

            char *angle_key = strstr(payload, "\"angle\"");
            if (angle_key == NULL)
            {
                ESP_LOGW(TAG, "angle not found in payload: %s", payload);
                break;
            }

            char *colon = strchr(angle_key, ':');
            if (colon == NULL)
            {
                ESP_LOGW(TAG, "invalid payload: %s", payload);
                break;
            }

            int angle = atoi(colon + 1);

            esp_err_t ret = sg90_set_angle((uint32_t)angle);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "sg90_set_angle failed: %s", esp_err_to_name(ret));
                break;
            }

            char status[32];
            snprintf(status, sizeof(status), "angle=%d", angle);
            ESP_LOGI(TAG, "SG90 set angle=%d from MQTT", angle);
        }
        else if (event->topic_len == strlen(MQTT_OTA_CMD_TOPIC) &&
                 strncmp(event->topic, MQTT_OTA_CMD_TOPIC, event->topic_len) == 0)
        {
            if (event->current_data_offset != 0 || event->data_len != event->total_data_len)
            {
                ESP_LOGW(TAG, "Ignore fragmented OTA payload");
                break;
            }

            char payload[32] = {0};
            int copy_len = event->data_len;
            if (copy_len >= (int)sizeof(payload))
            {
                copy_len = sizeof(payload) - 1;
            }
            memcpy(payload, event->data, copy_len);

            char *ota_key = strstr(payload, "\"ota\"");
            if (ota_key == NULL)
            {
                ESP_LOGW(TAG, "ota key not found in payload: %s", payload);
                break;
            }

            char *colon = strchr(ota_key, ':');
            if (colon == NULL)
            {
                ESP_LOGW(TAG, "invalid OTA payload: %s", payload);
                break;
            }

            int start = atoi(colon + 1);
            if (start == 1)
            {
                ESP_LOGI(TAG, "OTA start command received");
                ota_start();
            }
            else
            {
                ESP_LOGI(TAG, "OTA start value=%d ignored", start);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("esp-tls error", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("tls stack error", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGE(TAG, "socket errno text: %s", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        ESP_LOGD(TAG, "Unhandled MQTT event id=%d", event->event_id);
        break;
    }
}

void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .credentials.username = MQTT_BROKER_USERNAME,
        .credentials.authentication.password = MQTT_BROKER_PASSWORD,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return;
    }
}
