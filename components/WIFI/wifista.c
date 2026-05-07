#include "wifista.h"

static const char *TAG = "wifista";
static EventGroupHandle_t s_wifi_event_group;

static const char *wifi_disconnect_reason_to_string(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

void wifista_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    (void)event_handler_arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d(%s), rssi=%d, retrying",
                 event ? event->reason : -1,
                 event ? wifi_disconnect_reason_to_string(event->reason) : "UNKNOWN",
                 event ? event->rssi : 0);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, ip:" IPSTR ", gw:" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifista_init(void)
{
    esp_err_t ret;

    // 先准备 NVS。
    // Wi-Fi 驱动本身就依赖 NVS，如果这里没初始化好，后面联网也跑不起来。
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ret = nvs_flash_erase();
        if (ret != ESP_OK)
        {
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        return ret;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifista_event_handler, NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifista_event_handler, NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        return ret;
    }

    wifi_config_t wifista_config = { 0 };
    strlcpy((char *)wifista_config.sta.ssid, DEFAULT_SSID, sizeof(wifista_config.sta.ssid));
    strlcpy((char *)wifista_config.sta.password, DEFAULT_PWD, sizeof(wifista_config.sta.password));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // 这里把 STA 协议限制在 2.4G 下最常见、也最合法的 b/g/n 组合
    ret = esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // 带宽继续固定为 HT20，尽量让链路状态和后面的 CSI 使用场景保持一致
    ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifista_config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        return ret;
    }

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}
