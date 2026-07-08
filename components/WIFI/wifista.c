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
        ESP_LOGI(TAG, "Wi-Fi started, waiting for connect command");
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

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL)
    {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifista_base_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();
    assert(s_wifi_event_group);

    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifista_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifista_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(const char *ssid, const char *password)
{
    wifi_config_t wifista_config = {0};
    strlcpy((char *)wifista_config.sta.ssid, ssid, sizeof(wifista_config.sta.ssid));
    strlcpy((char *)wifista_config.sta.password, password, sizeof(wifista_config.sta.password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifista_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
        return ret;

    ret = nvs_set_str(handle, WIFI_NVS_SSID, ssid);
    if (ret == ESP_OK)
    {
        ret = nvs_set_str(handle, WIFI_NVS_PWD, password);
    }
    nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len, char *pwd, size_t pwd_len)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READONLY, &handle);
    if (ret != ESP_OK)
        return ret;

    size_t len = ssid_len;
    ret = nvs_get_str(handle, WIFI_NVS_SSID, ssid, &len);
    if (ret != ESP_OK)
    {
        nvs_close(handle);
        return ret;
    }

    len = pwd_len;
    ret = nvs_get_str(handle, WIFI_NVS_PWD, pwd, &len);
    nvs_close(handle);
    return ret;
}
