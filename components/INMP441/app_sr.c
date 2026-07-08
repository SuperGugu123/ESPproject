#include "app_sr.h"

static const char *TAG = "app_sr";

static model_iface_data_t *s_model_data = NULL;
static const esp_mn_iface_t *s_multinet = NULL;
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static QueueHandle_t s_result_que = NULL;
static srmodel_list_t *s_models = NULL;

static const char *const s_cmd_phoneme[] = {
    "da kai kong tiao",
    "guan bi kong tiao",
};

int16_t app_sr_convert_inmp441_sample(int32_t raw_sample)
{
    int32_t pcm = raw_sample >> APP_SR_INMP441_TO_PCM_SHIFT;

    if (pcm > INT16_MAX)
    {
        return INT16_MAX;
    }

    if (pcm < INT16_MIN)
    {
        return INT16_MIN;
    }

    return (int16_t)pcm;
}

void audio_feed_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    const int audio_chunksize = s_afe_handle->get_feed_chunksize(afe_data);
    const int feed_channel_num = s_afe_handle->get_feed_channel_num(afe_data);

    int32_t *raw_buffer = heap_caps_calloc(audio_chunksize, sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *feed_buffer = heap_caps_calloc(audio_chunksize * feed_channel_num,
                                            sizeof(int16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (raw_buffer == NULL || feed_buffer == NULL)
    {
        ESP_LOGE(TAG, "no memory for audio buffers");
        free(raw_buffer);
        free(feed_buffer);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed task start, chunksize=%d, channel_num=%d",
             audio_chunksize, feed_channel_num);

    while (true)
    {
        size_t samples_read = 0;
        esp_err_t ret = inmp441_read(raw_buffer, audio_chunksize, &samples_read, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "inmp441_read failed: %s", esp_err_to_name(ret));
            continue;
        }

        if (samples_read != (size_t)audio_chunksize)
        {
            ESP_LOGW(TAG, "short I2S read: %u/%d samples",
                     (unsigned int)samples_read, audio_chunksize);
            continue;
        }

        for (int i = 0; i < audio_chunksize; i++)
        {
            int16_t pcm = app_sr_convert_inmp441_sample(raw_buffer[i]);

            for (int ch = 0; ch < feed_channel_num; ch++)
            {
                feed_buffer[i * feed_channel_num + ch] = (ch == 0) ? pcm : 0;
            }
        }

        s_afe_handle->feed(afe_data, feed_buffer);
    }

    free(raw_buffer);
    free(feed_buffer);
    vTaskDelete(NULL);
}

void audio_detect_task(void *pvParam)
{
    bool detect_flag = false;
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;

    int afe_chunksize = s_afe_handle->get_fetch_chunksize(afe_data);
    int mn_chunksize = s_multinet->get_samp_chunksize(s_model_data);
    if (afe_chunksize != mn_chunksize)
    {
        ESP_LOGE(TAG, "chunksize mismatch: afe=%d, multinet=%d", afe_chunksize, mn_chunksize);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "detect task start, chunksize=%d", afe_chunksize);

    while (true)
    {
        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL)
        {
            ESP_LOGE(TAG, "fetch error!");
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "Wakeword detected");
            detect_flag = true;
            s_afe_handle->disable_wakenet(afe_data);

            sr_result_t result = {
                .wakenet_mode = WAKENET_DETECTED,
                .state = ESP_MN_STATE_DETECTING,
                .command_id = 0,
            };
            xQueueSend(s_result_que, &result, 10);
        }
        else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED)
        {
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "Channel verified");
            detect_flag = true;
            s_afe_handle->disable_wakenet(afe_data);
        }

        if (true == detect_flag)
        {
            esp_mn_state_t mn_state = ESP_MN_STATE_DETECTING;

            mn_state = s_multinet->detect(s_model_data, res->data);

            if (ESP_MN_STATE_DETECTING == mn_state)
            {
                continue;
            }

            if (ESP_MN_STATE_TIMEOUT == mn_state)
            {
                ESP_LOGW(TAG, "Time out");
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = 0,
                };
                xQueueSend(s_result_que, &result, 10);
                s_afe_handle->enable_wakenet(afe_data);
                detect_flag = false;
                continue;
            }

            if (ESP_MN_STATE_DETECTED == mn_state)
            {
                esp_mn_results_t *mn_result = s_multinet->get_results(s_model_data);
                for (int i = 0; i < mn_result->num; i++)
                {
                    ESP_LOGI(TAG, "TOP %d, command_id: %d, phrase_id: %d, prob: %f",
                             i + 1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->prob[i]);
                }

                int sr_command_id = mn_result->command_id[0];
                ESP_LOGI(TAG, "Deteted command : %d", sr_command_id);
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = sr_command_id,
                };
                xQueueSend(s_result_que, &result, 10);
#if !SR_CONTINUE_DET
                s_afe_handle->enable_wakenet(afe_data);
                detect_flag = false;
#endif
                continue;
            }

            ESP_LOGE(TAG, "Exception unhandled");
        }
    }

    s_afe_handle->destroy(afe_data);
    vTaskDelete(NULL);
}

void sr_handler_task(void *pvParam)
{
    QueueHandle_t result_queue = (QueueHandle_t)pvParam;
    sr_result_t result;

    while (true)
    {
        if (xQueueReceive(result_queue, &result, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        if (result.wakenet_mode == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, "handler: wake word detected");
        }
        else if (result.state == ESP_MN_STATE_TIMEOUT)
        {
            ESP_LOGI(TAG, "handler: command timeout");
        }
        else if (result.state == ESP_MN_STATE_DETECTED)
        {
            ESP_LOGI(TAG, "handler: command_id=%d", result.command_id);

            switch (result.command_id)
            {
            case 0:
                // 打开空调
                mqtt_sg90_on();
                break;

            case 1:
                // 关闭空调
                mqtt_sg90_off();
                break;

            default:
                ESP_LOGW(TAG, "unknown command_id=%d", result.command_id);
                break;
            }
        }
    }
}

esp_err_t app_sr_start(void)
{
    if (s_result_que != NULL)
    {
        ESP_LOGW(TAG, "speech recognition already started");
        return ESP_ERR_INVALID_STATE;
    }

    s_result_que = xQueueCreate(APP_SR_RESULT_QUEUE_LEN, sizeof(sr_result_t));
    ESP_RETURN_ON_FALSE(s_result_que != NULL, ESP_ERR_NO_MEM, TAG, "failed to create result queue");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models != NULL && s_models->num > 0,
                        ESP_ERR_NOT_FOUND,
                        TAG,
                        "no speech model loaded from model partition");

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size < APP_SR_MIN_PSRAM_SIZE)
    {
        ESP_LOGE(TAG, "PSRAM is required for current ESP-SR models, detected=%u bytes", (unsigned int)psram_size);
        ESP_LOGE(TAG, "enable CONFIG_SPIRAM or choose smaller WakeNet/MultiNet models");
        return ESP_ERR_NO_MEM;
    }

    afe_config_t *afe_config = afe_config_init(APP_SR_INPUT_FORMAT, s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(afe_config != NULL, ESP_ERR_NO_MEM, TAG, "failed to create AFE config");

    afe_config->wakenet_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    afe_config->aec_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    ESP_RETURN_ON_FALSE(s_afe_handle != NULL, ESP_FAIL, TAG, "failed to get AFE handle from config");

    ESP_RETURN_ON_FALSE(afe_config->wakenet_model_name != NULL,
                        ESP_ERR_NOT_FOUND,
                        TAG,
                        "no WakeNet model found; enable one CONFIG_SR_WN_* option");

    esp_afe_sr_data_t *afe_data = s_afe_handle->create_from_config(afe_config);
    ESP_LOGI(TAG, "load wakenet: %s", afe_config->wakenet_model_name);
    afe_config_free(afe_config);

    ESP_RETURN_ON_FALSE(afe_data != NULL, ESP_FAIL, TAG, "failed to create AFE data");

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_CHINESE, NULL);
    if (mn_name == NULL)
    {
        ESP_LOGE(TAG, "no Chinese MultiNet model found; enable CONFIG_SR_MN_CN_* in menuconfig");
        s_afe_handle->destroy(afe_data);
        return ESP_ERR_NOT_FOUND;
    }

    s_multinet = esp_mn_handle_from_name(mn_name);
    ESP_RETURN_ON_FALSE(s_multinet != NULL, ESP_ERR_NOT_FOUND, TAG, "failed to get multinet handle: %s", mn_name);

    s_model_data = s_multinet->create(mn_name, APP_SR_MN_TIMEOUT);
    ESP_RETURN_ON_FALSE(s_model_data != NULL, ESP_ERR_NO_MEM, TAG, "failed to create multinet model data");
    ESP_LOGI(TAG, "load multinet: %s", mn_name);

    ESP_RETURN_ON_ERROR(esp_mn_commands_alloc(s_multinet, s_model_data),
                        TAG,
                        "failed to allocate speech command list");

    for (int i = 0; i < (int)(sizeof(s_cmd_phoneme) / sizeof(s_cmd_phoneme[0])); i++)
    {
        ESP_RETURN_ON_ERROR(esp_mn_commands_add(i, s_cmd_phoneme[i]),
                            TAG,
                            "failed to add speech command: id=%d, %s",
                            i,
                            s_cmd_phoneme[i]);
    }

    esp_mn_error_t *cmd_err = esp_mn_commands_update();
    ESP_RETURN_ON_FALSE(cmd_err == NULL, ESP_FAIL, TAG, "failed to update speech command list");

    esp_mn_commands_print();
    s_multinet->print_active_speech_commands(s_model_data);

    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_feed_task,
                                                 "sr_feed",
                                                 APP_SR_FEED_TASK_STACK_SIZE,
                                                 afe_data,
                                                 5,
                                                 NULL,
                                                 1);
    ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "failed to create audio feed task");

    ret_val = xTaskCreatePinnedToCore(audio_detect_task,
                                      "sr_detect",
                                      APP_SR_DETECT_TASK_STACK_SIZE,
                                      afe_data,
                                      5,
                                      NULL,
                                      0);
    ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "failed to create audio detect task");

    ret_val = xTaskCreatePinnedToCore(sr_handler_task,
                                      "sr_handler",
                                      APP_SR_HANDLER_TASK_STACK_SIZE,
                                      s_result_que,
                                      1,
                                      NULL,
                                      1);
    ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "failed to create sr handler task");

    return ESP_OK;
}

esp_err_t app_sr_reset_command_list(char *command_list)
{
    if (command_list == NULL || command_list[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "app_sr_reset_command_list is not implemented yet: %s", command_list);
    return ESP_ERR_NOT_SUPPORTED;
}
