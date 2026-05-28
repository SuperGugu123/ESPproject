#include "app_ota.h"

// OTA 升级任务函数
static void ota_task(void *pvParameter)
{
    esp_err_t err;
    // OTA 更新句柄：由 esp_ota_begin() 设置，必须通过 esp_ota_end() 释放
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA example task");

    // 获取配置的启动分区和当前运行的分区
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    // 如果配置的启动分区与运行分区不一致，发出警告
    if (configured != running)
    {
        ESP_LOGW(TAG, "配置的 OTA 启动分区地址 0x%08" PRIx32 "，但实际运行在地址 0x%08" PRIx32,
                 configured->address, running->address);
        ESP_LOGW(TAG, "（这可能发生在 OTA 启动数据或首选启动镜像损坏的情况下。）");
    }
    ESP_LOGI(TAG, "运行分区类型 %d 子类型 %d（地址 0x%08" PRIx32 ")",
             running->type, running->subtype, running->address);

    // 配置 HTTP 客户端，从 menuconfig 获取固件 URL
    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPG_URL // http固件下载地址,
                   .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
    };
    // 初始化 HTTP 客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "HTTP 连接初始化失败");
        task_fatal_error();
    }
    err = esp_http_client_open(client, 0); // 打开 HTTP 连接
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP 连接打开失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        task_fatal_error();
    }
    esp_http_client_fetch_headers(client); // 获取 HTTP 响应头

    // 获取 OTA 更新的目标分区（第二个固件分区）
    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "写入分区子类型 %d，地址 0x%" PRIx32,
             update_partition->subtype, update_partition->address);

    int binary_file_length = 0;
    bool image_header_was_checked = false; // 标志：是否已检查固件头部

    // 循环接收固件数据
    while (1)
    {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0)
        {
            ESP_LOGE(TAG, "错误：SSL 数据读取失败");
            http_cleanup(client);
            task_fatal_error();
        }
        else if (data_read > 0)
        {
            // 首次读取：检查固件头部信息
            if (image_header_was_checked == false)
            {
                esp_app_desc_t new_app_info;
                // 确保收到的数据足够大，能包含固件头部和描述信息
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
                {
                    // 从下载数据中提取新固件信息
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "新固件版本: %s", new_app_info.version);

                    // 获取当前运行固件的信息
                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "当前运行固件版本: %s", running_app_info.version);
                    }

                    // 获取上次无效固件分区的信息
                    const esp_partition_t *last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "上次无效固件版本: %s", invalid_app_info.version);
                    }

                    // 检查新版本是否与上次无效版本相同（防止重复下载有问题的固件）
                    if (last_invalid_app != NULL)
                    {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0)
                        {
                            ESP_LOGW(TAG, "新版本与无效版本相同");
                            ESP_LOGW(TAG, "之前尝试启动此版本固件 %s 失败了", invalid_app_info.version);
                            ESP_LOGW(TAG, "固件已回滚到上一版本");
                            http_cleanup(client);
                            infinite_loop(); // 进入等待，不执行升级
                        }
                    }

                    // 检查新版本是否与当前运行版本相同
                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0)
                    {
                        ESP_LOGW(TAG, "新版本与当前运行版本相同，不进行升级");
                        http_cleanup(client);
                        infinite_loop();
                    }

                    image_header_was_checked = true;

                    // 开始 OTA 写入
                    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "esp_ota_begin 失败 (%s)", esp_err_to_name(err));
                        http_cleanup(client);
                        esp_ota_abort(update_handle);
                        task_fatal_error();
                    }
                    ESP_LOGI(TAG, "esp_ota_begin 成功");
                }
                else
                {
                    ESP_LOGE(TAG, "接收的数据包长度不足");
                    http_cleanup(client);
                    esp_ota_abort(update_handle);
                    task_fatal_error();
                }
            }
            // 写入固件数据到 OTA 分区
            err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK)
            {
                http_cleanup(client);
                esp_ota_abort(update_handle);
                task_fatal_error();
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "已写入固件长度 %d", binary_file_length);
        }
        else if (data_read == 0)
        {
            /*
             * esp_http_client_read 不会返回负数错误码，
             * 因此依赖 errno 检查底层传输连接是否关闭
             */
            if (errno == ECONNRESET || errno == ENOTCONN)
            {
                ESP_LOGE(TAG, "连接关闭，errno = %d", errno);
                break;
            }
            // 检查是否收到完整的固件数据
            if (esp_http_client_is_complete_data_received(client) == true)
            {
                ESP_LOGI(TAG, "连接关闭");
                break;
            }
        }
    }
    ESP_LOGI(TAG, "写入的二进制数据总长度: %d", binary_file_length);

    // 确保收到完整的固件文件
    if (esp_http_client_is_complete_data_received(client) != true)
    {
        ESP_LOGE(TAG, "接收完整文件时出错");
        http_cleanup(client);
        esp_ota_abort(update_handle);
        task_fatal_error();
    }

    // 结束 OTA 操作
    err = esp_ota_end(update_handle);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(TAG, "固件验证失败，镜像可能已损坏");
        }
        else
        {
            ESP_LOGE(TAG, "esp_ota_end 失败 (%s)!", esp_err_to_name(err));
        }
        http_cleanup(client);
        task_fatal_error();
    }

    // 设置启动分区为新下载的固件分区
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败 (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        task_fatal_error();
    }
    ESP_LOGI(TAG, "准备重启系统!");
    esp_restart(); // 重启以启动新固件
    return;
}

static bool diagnostic(void)
{
    return true;
}

// 应用主入口
void app_main(void)
{
    ESP_LOGI(TAG, "OTA 示例 app_main 开始");

    uint8_t sha_256[HASH_LEN] = {0};
    esp_partition_t partition;

    // 计算分区表的 SHA-256 摘要
    partition.address = ESP_PARTITION_TABLE_OFFSET;
    partition.size = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "分区表的 SHA-256: ");

    // 计算 Bootloader 的 SHA-256 摘要
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size = ESP_PARTITION_TABLE_OFFSET;
    partition.type = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "Bootloader 的 SHA-256: ");

    // 计算当前运行固件的 SHA-256 摘要
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "当前固件的 SHA-256: ");

    // 检查 OTA 状态，执行固件回滚检查
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
    {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            // 运行诊断函数验证新固件是否正常
            bool diagnostic_is_ok = diagnostic();
            if (diagnostic_is_ok)
            {
                ESP_LOGI(TAG, "诊断成功！继续执行...");
                esp_ota_mark_app_valid_cancel_rollback(); // 标记固件有效，取消回滚
            }
            else
            {
                ESP_LOGE(TAG, "诊断失败！回滚到上一版本...");
                esp_ota_mark_app_invalid_rollback_and_reboot(); // 标记固件无效并重启回滚
            }
        }
    }

    ESP_ERROR_CHECK(wifista_init());

    // 创建 OTA 升级任务，栈大小 8192，优先级 5
    xTaskCreate(&ota_example_task, "ota_task", 8192, NULL, 5, NULL);
}
