#include "lcd.h"

static const char *TAG = "LCD";

// ===== 文件内全局句柄（lcd_init 创建，getter 暴露给外部） =====
static esp_lcd_panel_handle_t _panel_handle = NULL; // ILI9341 面板驱动句柄
static esp_lcd_panel_io_handle_t _io_handle = NULL; // SPI Panel IO 句柄
static esp_lcd_touch_handle_t _touch_handle = NULL; // XPT2046 触摸驱动句柄

esp_lcd_panel_handle_t lcd_panel_handle(void)
{
    return _panel_handle;
}

esp_lcd_panel_io_handle_t lcd_io_handle(void)
{
    return _io_handle;
}

esp_lcd_touch_handle_t lcd_touch_handle(void)
{
    return _touch_handle;
}

// LCD 硬件初始化

esp_err_t lcd_init(void)
{
    static bool initialized = false;
    if (initialized)
    {
        ESP_LOGW(TAG, "LCD already initialized, skipping");
        return ESP_OK;
    }

    //  背光 GPIO（先关闭，面板就绪后再开）
    ESP_LOGI(TAG, "Init backlight GPIO");
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL);

    //  SPI 总线初始化
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,                          
        .mosi_io_num = PIN_NUM_MOSI,                         
        .miso_io_num = PIN_NUM_MISO,                          
        .quadwp_io_num = -1,                                  // 禁用 Quad SPI WP
        .quadhd_io_num = -1,                                  // 禁用 Quad SPI HD
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t), // 单次 DMA 最大传输字节数
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    //  创建 Panel IO（封装 SPI 命令/数据/DC/CS 控制）
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,    // D/C# 引脚
        .cs_gpio_num = PIN_NUM_LCD_CS,    // 片选引脚
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,    // SPI 时钟频率
        .lcd_cmd_bits = LCD_CMD_BITS,     // 命令位宽
        .lcd_param_bits = LCD_PARAM_BITS, // 参数位宽
        .spi_mode = 0,                    // SPI 模式 0 (CPOL=0, CPHA=0)
        .trans_queue_depth = 10,          // 事务队列深度
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &_io_handle));

    //  创建 ILI9341 面板驱动实例
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,          // 复位引脚
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // ILI9341 默认 BGR
        .bits_per_pixel = 16,                       // 16 位 = RGB565
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(_io_handle, &panel_config, &_panel_handle));

    //  复位 + 初始化序列（SLPOUT, 电源, 伽马等）
    ESP_ERROR_CHECK(esp_lcd_panel_reset(_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(_panel_handle));

    //  180° 旋转显示（X+Y 同时翻转）
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(_panel_handle, false, true));

    // 打开显示
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(_panel_handle, true));

    //  打开背光
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);

    initialized = true;
    ESP_LOGI(TAG, "LCD init success");
    return ESP_OK;
}

// 触摸硬件初始化

esp_lcd_touch_handle_t lcd_touch_init(void)
{
    if (_touch_handle)
    {
        ESP_LOGW(TAG, "Touch already initialized, skipping");
        return _touch_handle;
    }

    ESP_LOGI(TAG, "Init XPT2046 touch controller");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1, 
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = {
        .cs_gpio_num = PIN_NUM_TOUCH_CS,
        .pclk_hz = 1 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_cfg, &tp_io));
    esp_err_t tp_err = esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &_touch_handle);
    ESP_LOGI(TAG, "Touch init result: 0x%x handle=%p", tp_err, _touch_handle);
    ESP_ERROR_CHECK(tp_err);
    return _touch_handle;
}
