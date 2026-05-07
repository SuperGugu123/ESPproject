#include "app_mqtt.h"

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

/*
 * ESP-IDF 的 MQTT 客户端是异步工作的：
 * 1. mqtt_app_start() 只负责创建客户端并启动 MQTT 后台任务。
 * 2. 真正的联网、握手、重连都在 MQTT 组件内部异步完成。
 * 3. mqtt_event_handler() 会在连接状态变化时更新 mqtt_connected。
 *
 * 所有发布函数都会先检查 mqtt_connected，避免设备离线时还继续发送命令。
 */
static bool mqtt_connected = false;
static esp_mqtt_client_handle_t client = NULL;

/*
 * topic:
 *   Home Assistant 侧监听的 MQTT 主题，可以由 HA 自动化、MQTT 实体或
 *   Node-RED 等服务消费。
 *
 * payload:
 *   发送给 HA 的 JSON 命令内容。这里沿用 smart_home_panel 示例里的格式：
 *   input 表示动作语义，siteId 表示命令来源设备。
 *
 * QoS:
 *   这里使用 qos=1，表示消息至少送达 broker 一次，broker 会返回确认。
 *
 * retain:
 *   这里使用 retain=0，因为这些是"动作命令"，不应该作为保留状态留在 broker。
 */
esp_err_t mqtt_publish_command(const char *topic, const char *payload)
{
    ESP_RETURN_ON_FALSE(mqtt_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_STATE, TAG, "MQTT client is not initialized");

    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "Publish failed, topic=%s, msg_id=%d", topic, msg_id);

    ESP_LOGI(TAG, "Publish queued, topic=%s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

/* 发送命令。 */
esp_err_t mqtt_sg90_on(void)
{
    return mqtt_publish_command(
        "sg90/control",
        "{\"input\": \"sg90 on\", \"siteId\": \"esp32\"}");
}

esp_err_t mqtt_sg90_off(void)
{
    return mqtt_publish_command(
        "sg90/control",
        "{\"input\": \"sg90 off\", \"siteId\": \"esp32\"}");
}

bool mqtt_is_connected(void)
{
    return mqtt_connected;
}

/*
 * MQTT 错误里有多层错误码：
 * - esp_tls_last_esp_err：ESP-TLS 层错误。
 * - esp_tls_stack_err：TLS 协议栈错误。
 * - esp_transport_sock_errno：socket 层 errno。
 *
 * 没有错误时这些字段通常是 0，所以这里统一过滤掉无效错误码。
 */
void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "%s: 0x%x", message, error_code);
    }
}

/*
 * MQTT 事件回调函数。
 *
 * ESP-IDF 的 MQTT 组件会在内部事件循环里调用这个函数。这里不要做耗时操作，
 * 只做状态更新、日志输出和简单的数据分发。
 */
void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;

    ESP_LOGD(TAG, "Event dispatched, base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        /*
         * MQTT_EVENT_CONNECTED 表示 TCP 连接和 MQTT 握手已经完成。
         * 从这个事件之后，发布函数才可以正常向 broker 发送消息。
         */
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        {
            int msg_id = esp_mqtt_client_subscribe(client, MQTT_SG90_CMD_TOPIC, 1);
            if (msg_id < 0)
            {
                ESP_LOGE(TAG, "Subscribe failed, topic=%s", MQTT_SG90_CMD_TOPIC);
            }
            else
            {
                ESP_LOGI(TAG, "Subscribe queued, topic=%s, msg_id=%d", MQTT_SG90_CMD_TOPIC, msg_id);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        /*
         * 断开连接后，ESP-IDF MQTT 客户端默认会尝试自动重连。
         * 这里把状态标记为离线，让上层逻辑可以稍后重试。
         */
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        /* 订阅成功事件。当前代码暂时没有主动订阅主题，保留日志方便以后扩展。 */
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        /* 取消订阅成功事件。 */
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        /*
         * 对 QoS 1 消息来说，这表示 broker 已经确认收到消息。
         * 注意：这不等于 Home Assistant 已经完成动作，只表示 broker 收到了。
         */
        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        /*
         * 如果后续要订阅 HA 下发的控制主题，收到的数据会进入这里。
         * event->topic 和 event->data 不是以 '\0' 结尾的字符串，所以打印时必须
         * 使用 %.*s 并传入长度。
         */
        ESP_LOGI(TAG, "MQTT data received");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

        /*
         * 先判断本次收到的 MQTT 消息是不是我们关心的控制主题：
         *   esp32/sg90/cmd
         *
         * 这里不能直接用 strcmp(event->topic, "...")，
         * 因为 event->topic 不是以 '\0' 结尾的标准 C 字符串，
         * 所以要先比较长度，再用 strncmp 按指定长度比较内容。
         *
         * 只有当主题完全匹配时，才继续往下解析载荷并控制舵机。
         */
        if (event->topic_len == strlen(MQTT_SG90_CMD_TOPIC) &&
            strncmp(event->topic, MQTT_SG90_CMD_TOPIC, event->topic_len) == 0)
        {
            /*
             * MQTT 有可能把一条较长消息拆成多段回调给我们。
             * 这段代码只处理“完整的一条消息”：
             * - current_data_offset == 0 说明这是从第 0 个字节开始
             * - data_len == total_data_len 说明这一段就是全部数据
             *
             * 如果消息被分片了，这里先不处理，直接打印警告后退出，
             * 避免我们只拿到半截 JSON 就开始解析，导致结果出错。
             */
            if (event->current_data_offset != 0 || event->data_len != event->total_data_len)
            {
                ESP_LOGW(TAG, "Ignore fragmented MQTT payload");
                break;
            }

            /*
             * event->data 同样不是以 '\0' 结尾的字符串，
             * 所以先把它拷贝到本地缓冲区 payload 里，
             * 再手动补上字符串结尾 '\0'，这样后面才能安全地使用
             * strstr / strchr / atoi 这些字符串处理函数。
             *
             * 这里把缓冲区长度限制为 64 字节，足够放类似：
             *   {"angle":90}
             * 这样的简单控制命令。
             */
            char payload[64] = {0};
            int copy_len = event->data_len;
            if (copy_len >= (int)sizeof(payload))
            {
                /* 如果收到的内容太长，就只截取前 63 个字节，留 1 个字节给 '\0' */
                copy_len = sizeof(payload) - 1;
            }
            memcpy(payload, event->data, copy_len);

            /*
             * 在 JSON 字符串里查找 "angle" 这个键。
             * 比如收到：
             *   {"angle":90}
             * 那么 angle_key 会指向 "angle" 这几个字符的位置。
             *
             * 如果找不到，说明这条消息不是我们预期的格式，
             * 就打印警告并退出，不做舵机控制。
             */
            char *angle_key = strstr(payload, "\"angle\"");
            if (angle_key == NULL)
            {
                ESP_LOGW(TAG, "angle not found in payload: %s", payload);
                break;
            }

            /*
             * 找到 "angle" 后，继续找它后面的冒号 ':'。
             * 只有找到冒号，才能继续把冒号后面的内容当成数值来解析。
             *
             * 如果连冒号都没有，说明这个 JSON 格式不完整或不正确，
             * 例如：
             *   {"angle" 90}
             * 这种情况就直接退出。
             */
            char *colon = strchr(angle_key, ':');
            if (colon == NULL)
            {
                ESP_LOGW(TAG, "invalid payload: %s", payload);
                break;
            }

            /*
             * atoi(colon + 1) 的意思是：
             * 从冒号后面开始，把后面的文本转换成整数。
             *
             * 例如 payload 是：
             *   {"angle":90}
             * 那么 colon + 1 指向的就是：
             *   90}
             * atoi 会从开头连续读取数字部分，得到 90。
             */
            int angle = atoi(colon + 1);

            /*
             * 调用舵机驱动接口，直接把舵机转到指定角度。
             * 这里调用的是最基础的 sg90_set_angle()，
             * 不做额外优先级处理，就是“收到命令就设置角度”。
             */
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
        break;

    case MQTT_EVENT_ERROR:
        /*
         * 连接失败、认证失败、DNS 失败、socket 断开等问题通常都会走到这里。
         * 下面拆出底层错误码，方便从串口日志判断是 broker 地址、账号密码，
         * 还是网络本身出了问题。
         */
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
        /* 其他事件暂时不处理，保留 debug 日志方便排查。 */
        ESP_LOGD(TAG, "Unhandled MQTT event id=%d", event->event_id);
        break;
    }
}

void mqtt_app_start(void)
{
    /*
     * CONFIG_BROKER_URL / CONFIG_BROKER_USERNAME / CONFIG_BROKER_PASSWORD
     * 来自 main/Kconfig.projbuild。
     *
     * 修改方式：
     *   idf.py menuconfig
     *   Example Configuration
     *
     * 常见 Home Assistant Mosquitto 地址：
     *   mqtt://homeassistant.local:1883
     */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .credentials.username = CONFIG_BROKER_USERNAME,
        .credentials.authentication.password = CONFIG_BROKER_PASSWORD,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    /*
     * 注册所有 MQTT 事件。
     * 这样连接状态、发布确认、接收数据、错误诊断都集中在同一个回调里处理。
     */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /*
     * 启动 MQTT 后台任务。
     * 这个函数返回 ESP_OK 只代表任务启动成功，不代表已经连上 broker。
     * 是否真正连接成功，要看后续是否收到 MQTT_EVENT_CONNECTED。
     */
    esp_err_t ret = esp_mqtt_client_start(client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return;
    }
}
