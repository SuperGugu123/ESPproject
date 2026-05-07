#include "inmp441.h"

static const char *TAG = "INMP441";

static i2s_chan_handle_t s_rx_chan;

esp_err_t inmp441_init(void)
{
    esp_err_t ret;

    // 创建 I2S 通道基础配置。
    // I2S_NUM_AUTO 表示让 ESP-IDF 自动选择一个空闲的 I2S 外设控制器。
    // I2S_ROLE_MASTER 表示 ESP32 作为主机输出 BCLK 和 WS 时钟，INMP441 跟随这些时钟输出音频数据。
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    // 配置 DMA 缓冲区数量。I2S 音频数据是连续输入的，DMA 会自动把数据搬到内存，
    // desc_num 越大越不容易因为处理不及时而丢数据，但会占用更多内存。
    chan_cfg.dma_desc_num = INMP441_DMA_DESC_NUM;

    // 配置每个 DMA 描述符中包含的音频帧数量。
    // frame_num 越大，单次缓冲的数据越多，采集更稳定，但读取延迟也会相应增加。
    chan_cfg.dma_frame_num = INMP441_DMA_FRAME_NUM;

    // 根据上面的通道配置申请一个 I2S 接收通道。
    // 第二个参数传 NULL，表示不创建 TX 发送通道；第三个参数保存 RX 接收通道句柄。
    // 如果申请失败，ESP_RETURN_ON_ERROR 会打印错误日志并直接从当前函数返回错误码。
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
                        TAG,
                        "failed to allocate I2S RX channel");

    // 配置 I2S 标准模式参数。
    // INMP441 使用 I2S Philips 标准格式，这里设置采样率、数据位宽、声道模式和 GPIO 引脚。
    i2s_std_config_t std_cfg = {
        // 设置 I2S 时钟，采样率由 INMP441_SAMPLE_RATE_HZ 指定。
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INMP441_SAMPLE_RATE_HZ),

        // 设置 I2S 数据槽格式：32 位数据宽度、单声道模式。
        // INMP441 常见输出为 24 位有效数据放在 32 位 I2S 帧中，因此这里使用 32bit 读取。
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),

        // 设置 I2S 相关 GPIO。
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,

            .bclk = INMP441_SCK_GPIO,

            .ws = INMP441_WS_GPIO,

            .dout = I2S_GPIO_UNUSED,

            .din = INMP441_SD_GPIO,
        },
    };

    // INMP441 的 L/R 引脚如果接 GND，数据会输出在左声道槽位。
    // 因此这里选择读取 LEFT 槽位；如果 L/R 接 VDD，通常应改为 I2S_STD_SLOT_RIGHT。
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    // 用上面的标准模式配置初始化 RX 通道。
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to initialize I2S STD mode: %s", esp_err_to_name(ret));

        // 初始化失败时释放刚才申请的通道，并清空句柄，避免后续误认为已经初始化成功。
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    // 启动 I2S RX 通道。启用之后，I2S/DMA 才会开始接收麦克风数据。
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to enable I2S RX channel: %s", esp_err_to_name(ret));

        // 启用失败同样需要释放通道资源，保证下次可以重新初始化。
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    return ESP_OK;
}

esp_err_t inmp441_read(int32_t *samples,
                       size_t sample_count,
                       size_t *samples_read,
                       TickType_t timeout_ticks)
{
    if (samples == NULL || samples_read == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_rx_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan,
                                     samples,
                                     sample_count * sizeof(samples[0]),
                                     &bytes_read,
                                     timeout_ticks);
    *samples_read = bytes_read / sizeof(samples[0]);
    return ret;
}
