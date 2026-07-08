#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#define MQTT_BROKER_URL "mqtt://192.168.31.25:1883" // MQTT 服务器地址
#define MQTT_BROKER_USERNAME "supergu"              // MQTT 服务器用户名
#define MQTT_BROKER_PASSWORD "1284529154"           // MQTT 服务器密码

void mqtt_app_start(void);
esp_err_t mqtt_publish_command(const char *topic, const char *payload);
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void log_error_if_nonzero(const char *message, int error_code);
esp_err_t mqtt_sg90_off(void);
esp_err_t mqtt_sg90_on(void);
esp_err_t mqtt_radar_state_empty(void);
esp_err_t mqtt_radar_state_use(void);
bool mqtt_is_connected(void);

#endif
