#include "radar.h"

#include <stdlib.h>

const char *g_radar_tag = "radar_app";

typedef struct
{
    uint32_t magic;
    uint32_t version;
    float wander_threshold;
    float jitter_threshold;
} radar_threshold_store_v1_t;

float g_wander_threshold = 0.0f;
float g_jitter_threshold = 0.0f;
float g_no_person_wander_baseline = 0.0f;
float g_static_jitter_baseline = 0.0f;
bool g_thresholds_ready = false;
bool g_training_in_progress = false;
radar_state_t g_radar_state = RADAR_STATE_EMPTY;
radar_state_t g_radar_candidate_state = RADAR_STATE_EMPTY;
uint8_t g_radar_candidate_count = 0;

static float radar_get_presence_signal(float wander, float jitter, float motion_enter_threshold);
static esp_err_t radar_save_train_data_to_nvs(void);
static esp_err_t radar_load_train_data_from_nvs(void);

const char *radar_state_to_string(radar_state_t state)
{
    switch (state) {
    case RADAR_STATE_PRESENCE:
        return "presence";
    case RADAR_STATE_MOTION:
        return "motion";
    case RADAR_STATE_EMPTY:
    default:
        return "empty";
    }
}

float radar_get_effective_threshold(float raw_threshold, float scale)
{
    if (raw_threshold <= 0.05f) {
        return raw_threshold;
    }

    return raw_threshold * scale;
}

void radar_reset_state_machine(void)
{
    g_radar_state = RADAR_STATE_EMPTY;
    g_radar_candidate_state = RADAR_STATE_EMPTY;
    g_radar_candidate_count = 0;
}

static float radar_get_presence_signal(float wander, float jitter, float motion_enter_threshold)
{
    if (wander > 0.0001f) {
        return wander;
    }

    if (jitter >= RADAR_PRESENCE_JITTER_FALLBACK_MIN && jitter < motion_enter_threshold) {
        return RADAR_WANDER_FALLBACK_VALUE;
    }

    return wander;
}

static esp_err_t radar_save_train_data_to_nvs(void)
{
    nvs_handle_t handle;
    void *train_data = NULL;
    size_t train_data_size = 0;
    esp_err_t ret = esp_radar_get_train_data_size(&train_data_size);

    if (ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "unable to get train data size: %s", esp_err_to_name(ret));
        return ret;
    }

    train_data = malloc(train_data_size);
    if (train_data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_radar_export_train_data(train_data, train_data_size, NULL);
    if (ret != ESP_OK) {
        free(train_data);
        ESP_LOGW(g_radar_tag, "unable to export train data: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, RADAR_TRAIN_DATA_KEY, train_data, train_data_size);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    free(train_data);

    if (ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "save train data failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(g_radar_tag, "train data saved to NVS (%u bytes)", (unsigned)train_data_size);
    return ESP_OK;
}

static esp_err_t radar_load_train_data_from_nvs(void)
{
    nvs_handle_t handle;
    void *train_data = NULL;
    size_t train_data_size = 0;
    esp_err_t ret = nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_blob(handle, RADAR_TRAIN_DATA_KEY, NULL, &train_data_size);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    train_data = malloc(train_data_size);
    if (train_data == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_blob(handle, RADAR_TRAIN_DATA_KEY, train_data, &train_data_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        free(train_data);
        return ret;
    }

    ret = esp_radar_import_train_data(train_data, train_data_size);
    free(train_data);
    if (ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "restore train data failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(g_radar_tag, "train data restored from NVS (%u bytes)", (unsigned)train_data_size);
    return ESP_OK;
}

esp_err_t radar_save_thresholds_to_nvs(void)
{
    nvs_handle_t handle;
    radar_threshold_store_t store = {
        .magic = RADAR_THRESHOLD_MAGIC,
        .version = 2,
        .wander_threshold = g_wander_threshold,
        .jitter_threshold = g_jitter_threshold,
        .no_person_wander_baseline = g_no_person_wander_baseline,
        .static_jitter_baseline = g_static_jitter_baseline,
    };

    esp_err_t ret = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(g_radar_tag, "open NVS for save failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, RADAR_NVS_KEY, &store, sizeof(store));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(g_radar_tag, "save thresholds to NVS failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(g_radar_tag, "thresholds saved to NVS");
    return ESP_OK;
}

esp_err_t radar_load_thresholds_from_nvs(void)
{
    nvs_handle_t handle;
    radar_threshold_store_t store = {0};
    radar_threshold_store_v1_t store_v1 = {0};
    size_t required_size = sizeof(store);

    esp_err_t ret = nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "open NVS for load failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_get_blob(handle, RADAR_NVS_KEY, &store, &required_size);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "load thresholds from NVS failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (required_size == sizeof(store) && store.magic == RADAR_THRESHOLD_MAGIC && store.version == 2) {
        g_wander_threshold = store.wander_threshold;
        g_jitter_threshold = store.jitter_threshold;
        g_no_person_wander_baseline = store.no_person_wander_baseline;
        g_static_jitter_baseline = store.static_jitter_baseline;
    } else if (required_size == sizeof(store_v1)) {
        memcpy(&store_v1, &store, sizeof(store_v1));
        if (store_v1.magic != RADAR_THRESHOLD_MAGIC || store_v1.version != 1) {
            ESP_LOGW(g_radar_tag, "legacy NVS threshold data is invalid");
            return ESP_ERR_INVALID_RESPONSE;
        }
        g_wander_threshold = store_v1.wander_threshold;
        g_jitter_threshold = store_v1.jitter_threshold;
        g_no_person_wander_baseline = 1.0f - g_wander_threshold;
        g_static_jitter_baseline = 1.0f - g_jitter_threshold;
    } else {
        ESP_LOGW(g_radar_tag, "NVS threshold data is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }

    g_thresholds_ready = true;
    radar_reset_state_machine();

    ESP_LOGI(g_radar_tag,
             "thresholds loaded from NVS: wander=%.3f jitter=%.3f baseline_w=%.3f baseline_j=%.3f",
             g_wander_threshold,
             g_jitter_threshold,
             g_no_person_wander_baseline,
             g_static_jitter_baseline);
    return ESP_OK;
}

radar_state_t radar_get_desired_state(float wander,
                                      float jitter,
                                      float presence_enter_threshold,
                                      float presence_exit_threshold,
                                      float motion_enter_threshold,
                                      float motion_exit_threshold)
{
    if (g_radar_state == RADAR_STATE_MOTION) {
        if (jitter > motion_exit_threshold) {
            return RADAR_STATE_MOTION;
        }
        if (wander > presence_enter_threshold || wander > presence_exit_threshold) {
            return RADAR_STATE_PRESENCE;
        }
        return RADAR_STATE_EMPTY;
    }

    if (g_radar_state == RADAR_STATE_PRESENCE) {
        if (jitter > motion_enter_threshold) {
            return RADAR_STATE_MOTION;
        }
        if (wander > presence_exit_threshold) {
            return RADAR_STATE_PRESENCE;
        }
        return RADAR_STATE_EMPTY;
    }

    if (jitter > motion_enter_threshold) {
        return RADAR_STATE_MOTION;
    }
    if (wander > presence_enter_threshold) {
        return RADAR_STATE_PRESENCE;
    }
    return RADAR_STATE_EMPTY;
}

void radar_update_stable_state(radar_state_t desired_state)
{
    const uint8_t required_confirm_frames =
        (desired_state == RADAR_STATE_EMPTY) ? RADAR_EMPTY_CONFIRM_FRAMES : RADAR_STATE_CONFIRM_FRAMES;

    if (desired_state == g_radar_state) {
        g_radar_candidate_state = g_radar_state;
        g_radar_candidate_count = 0;
        return;
    }

    if (desired_state == g_radar_candidate_state) {
        if (g_radar_candidate_count < UINT8_MAX) {
            g_radar_candidate_count++;
        }
    } else {
        g_radar_candidate_state = desired_state;
        g_radar_candidate_count = 1;
    }

    if (g_radar_candidate_count >= required_confirm_frames) {
        g_radar_state = desired_state;
        g_radar_candidate_state = g_radar_state;
        g_radar_candidate_count = 0;
    }
}

void radar_rx_cb(void *ctx, const wifi_radar_info_t *info)
{
    (void)ctx;

    if (g_training_in_progress || !g_thresholds_ready) {
        ESP_LOGD(g_radar_tag, "state=training wander=%.3f jitter=%.3f",
                 info->waveform_wander, info->waveform_jitter);
        return;
    }

    const float presence_enter_threshold = radar_get_effective_threshold(g_wander_threshold, RADAR_PRESENCE_SCALE);
    const float motion_enter_threshold = radar_get_effective_threshold(g_jitter_threshold, RADAR_MOTION_SCALE);
    const float presence_exit_threshold = presence_enter_threshold * RADAR_PRESENCE_EXIT_SCALE;
    const float motion_exit_threshold = motion_enter_threshold * RADAR_MOTION_EXIT_SCALE;
    const float presence_signal = radar_get_presence_signal(info->waveform_wander,
                                                            info->waveform_jitter,
                                                            motion_enter_threshold);

    radar_state_t desired_state = radar_get_desired_state(presence_signal,
                                                          info->waveform_jitter,
                                                          presence_enter_threshold,
                                                          presence_exit_threshold,
                                                          motion_enter_threshold,
                                                          motion_exit_threshold);

    radar_update_stable_state(desired_state);

    ESP_LOGD(g_radar_tag,
             "state=%s desired=%s confirm=%u wander=%.3f presence_sig=%.3f jitter=%.3f th_w=%.3f th_j=%.3f exit_w=%.3f exit_j=%.3f raw_w=%.3f raw_j=%.3f",
             radar_state_to_string(g_radar_state),
             radar_state_to_string(desired_state),
             g_radar_candidate_count,
             info->waveform_wander,
             presence_signal,
             info->waveform_jitter,
             presence_enter_threshold,
             motion_enter_threshold,
             presence_exit_threshold,
             motion_exit_threshold,
             g_wander_threshold,
             g_jitter_threshold);
}

esp_err_t radar_init(void)
{
    wifi_ap_record_t ap_info = {0};
    esp_radar_csi_config_t csi_config = ESP_RADAR_CSI_CONFIG_DEFAULT();
    esp_radar_dec_config_t dec_config = ESP_RADAR_DEC_CONFIG_DEFAULT();
    esp_err_t load_ret;
    esp_err_t train_data_ret;

    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));

    memcpy(csi_config.filter_mac, ap_info.bssid, sizeof(ap_info.bssid));

    csi_config.csi_compensate_en = true;
    csi_config.lltf_en = true;
    csi_config.htltf_en = false;
    csi_config.stbc_htltf2_en = false;
    csi_config.ltf_merge_en = true;
    csi_config.channel_filter_en = true;
    csi_config.manu_scale = true;
    csi_config.shift = 1;
    csi_config.dump_ack_en = false;

    ESP_LOGI(g_radar_tag,
             "csi cfg: lltf=%d htltf=%d stbc=%d merge=%d ch_filter=%d manu_scale=%d shift=%u",
             csi_config.lltf_en,
             csi_config.htltf_en,
             csi_config.stbc_htltf2_en,
             csi_config.ltf_merge_en,
             csi_config.channel_filter_en,
             csi_config.manu_scale,
             csi_config.shift);

    dec_config.wifi_radar_cb = radar_rx_cb;

    ESP_ERROR_CHECK(esp_radar_csi_init(&csi_config));
    ESP_ERROR_CHECK(esp_radar_dec_init(&dec_config));

    load_ret = radar_load_thresholds_from_nvs();
    if (load_ret != ESP_OK) {
        ESP_LOGW(g_radar_tag, "thresholds are not ready yet, training is required");
    } else {
        train_data_ret = radar_load_train_data_from_nvs();
        if (train_data_ret != ESP_OK) {
            ESP_LOGW(g_radar_tag, "train data is not ready yet, wander may stay zero until retraining");
        }
    }

    ESP_ERROR_CHECK(esp_radar_start());
    return ESP_OK;
}

esp_err_t radar_train(uint32_t train_ms)
{
    g_thresholds_ready = false;
    g_training_in_progress = true;
    radar_reset_state_machine();

    ESP_ERROR_CHECK(esp_radar_train_start());
    vTaskDelay(pdMS_TO_TICKS(train_ms));
    ESP_ERROR_CHECK(esp_radar_train_stop(&g_wander_threshold, &g_jitter_threshold));

    g_training_in_progress = false;
    g_thresholds_ready = true;
    radar_reset_state_machine();
    g_no_person_wander_baseline = 1.0f - g_wander_threshold;
    g_static_jitter_baseline = 1.0f - g_jitter_threshold;

    ESP_LOGI(g_radar_tag,
             "train done raw_wander_th=%.3f raw_jitter_th=%.3f baseline_w=%.3f baseline_j=%.3f effective_wander_th=%.3f effective_jitter_th=%.3f empty_confirm=%u",
             g_wander_threshold,
             g_jitter_threshold,
             g_no_person_wander_baseline,
             g_static_jitter_baseline,
             radar_get_effective_threshold(g_wander_threshold, RADAR_PRESENCE_SCALE),
             radar_get_effective_threshold(g_jitter_threshold, RADAR_MOTION_SCALE),
             RADAR_EMPTY_CONFIRM_FRAMES);

    ESP_ERROR_CHECK(radar_save_thresholds_to_nvs());
    return radar_save_train_data_to_nvs();
}

bool radar_thresholds_ready(void)
{
    return g_thresholds_ready;
}

radar_state_t radar_get_state(void)
{
    return g_radar_state;
}

float radar_get_no_person_wander_baseline(void)
{
    return g_no_person_wander_baseline;
}

float radar_get_static_jitter_baseline(void)
{
    return g_static_jitter_baseline;
}
