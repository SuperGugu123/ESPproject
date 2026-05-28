#include "sg90.h"

static const char *TAG = "SG90";
static SemaphoreHandle_t s_sg90_mutex = NULL;
static uint32_t s_current_angle = SG90_MIN_ANGLE;
static TickType_t s_voice_override_until = 0;

uint32_t sg90_clamp_angle(uint32_t angle)
{
    if (angle > SG90_MAX_ANGLE)
    {
        return SG90_MAX_ANGLE;
    }

    return angle;
}

esp_err_t sg90_apply_angle(uint32_t angle)
{
    const uint32_t pulse_us = SG90_MIN_PULSE_US +
                              (SG90_MAX_PULSE_US - SG90_MIN_PULSE_US) * angle / SG90_MAX_ANGLE;
    const uint32_t duty = (pulse_us * ((1 << LEDC_TIMER_10_BIT) - 1)) / SG90_PERIOD_US;

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    if (err != ESP_OK)
    {
        return err;
    }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "set angle=%" PRIu32 ", pulse=%" PRIu32 "us, duty=%" PRIu32,
             angle, pulse_us, duty);
    return ESP_OK;
}

void sg90_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .clk_cfg = LEDC_AUTO_CLK,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 50,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_1,
        .duty = 0,
        .flags.output_invert = 0,
        .gpio_num = SG90_PWM_PIN,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_1,
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ledc_channel_config failed (GPIO %d): %s",
                 SG90_PWM_PIN, esp_err_to_name(err));
        return;
    }

    if (s_sg90_mutex == NULL)
    {
        s_sg90_mutex = xSemaphoreCreateMutex();
        if (s_sg90_mutex == NULL)
        {
            ESP_LOGE(TAG, "create mutex failed");
            return;
        }
    }

    err = sg90_apply_angle(SG90_MIN_ANGLE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "apply initial angle failed: %s", esp_err_to_name(err));
        return;
    }

    s_current_angle = SG90_MIN_ANGLE;
    ESP_LOGI(TAG, "init done on GPIO %d", SG90_PWM_PIN);
}

esp_err_t sg90_set_angle(uint32_t angle)
{
    const uint32_t clamped_angle = sg90_clamp_angle(angle);

    if (s_sg90_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_sg90_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_current_angle == clamped_angle)
    {
        xSemaphoreGive(s_sg90_mutex);
        return ESP_OK;
    }

    esp_err_t err = sg90_apply_angle(clamped_angle);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_sg90_mutex);
        return err;
    }

    s_current_angle = clamped_angle;
    xSemaphoreGive(s_sg90_mutex);

    return ESP_OK;
}

esp_err_t sg90_set_angle_from_radar(uint32_t angle)
{
    return sg90_set_angle(angle);
}

uint32_t sg90_get_angle(void)
{
    return s_current_angle;
}

void sg90_angle(uint32_t sr04_distance)
{
    esp_err_t err = ESP_OK;

    if (sr04_distance > SG90_DISTANCE_THRESHOLD_MM)
    {
        err = sg90_set_angle(SG90_DEFAULT_OPEN_ANGLE);
    }
    else
    {
        err = sg90_set_angle(SG90_MIN_ANGLE);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set angle by distance failed: %s", esp_err_to_name(err));
    }
}
