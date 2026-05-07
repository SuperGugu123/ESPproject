#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

void mqtt_app_start(void);
esp_err_t mqtt_publish_command(const char *topic, const char *payload);
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void log_error_if_nonzero(const char *message, int error_code);
esp_err_t mqtt_sg90_off(void);
esp_err_t mqtt_sg90_on(void);
bool mqtt_is_connected(void);

#endif
