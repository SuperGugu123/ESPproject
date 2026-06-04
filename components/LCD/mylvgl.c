#include "mylvgl.h"

static const char *TAG = "LVGL";

// ===== 互斥锁（LVGL 非线程安全，需保护） =====
static _lock_t lvgl_api_lock;

// ===== 全局对象 =====
static lv_display_t *lvgl_disp = NULL; // 显示设备
static lv_obj_t *main_screen = NULL;   // 主界面
static lv_obj_t *home_screen = NULL;   // 家居界面
static lv_obj_t *time_label = NULL;    // 时间标签
static bool ntp_synced = false;        // NTP 是否已同步

// SPI 颜色传输完成回调：DMA 传输结束后通知 LVGL 刷屏完成
static bool lvgl_port_update_callback(esp_lcd_panel_io_handle_t io,
                                      esp_lcd_panel_io_event_data_t *edata,
                                      void *user_data)
{
    lv_display_flush_ready((lv_display_t *)user_data);
    return false;
}

// LVGL 刷屏回调：把像素数据通过 SPI 发送到 LCD
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    // LVGL 内部用 little-endian RGB565，SPI LCD 需 big-endian
    lv_draw_sw_rgb565_swap(px_map, w * h);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

// ==================== NTP 北京时间 ====================

// NTP 初始化（需在 WiFi 连接后调用）
void lvgl_ntp_init(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();
    ntp_synced = true;
}

// LVGL 定时器：每秒更新一次时间显示
void time_update_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    lv_label_set_text(time_label, buf);
}

// ==================== 退出回调 ====================
void exit_cb(lv_event_t *e)
{
    ESP_LOGW(TAG, "=== EXIT btn pressed ===");
    lv_obj_clean(lv_scr_act());
    esp_lcd_panel_disp_on_off(lcd_panel_handle(), false);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// ==================== 家居界面回调 ====================
void light_btn_cb(lv_event_t *e)
{
    static bool light_on = false;
    lv_obj_t *btn = lv_event_get_target(e);
    ESP_LOGI(TAG, "=== LIGHT btn pressed ===");

    if (light_on)
    {
        mqtt_sg90_off();
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DEFAULT);
    }
    else
    {
        mqtt_sg90_on();
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_DEFAULT);
    }
    light_on = !light_on;
}

void ret_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "=== RETURN btn pressed ===");
    lv_scr_load(main_screen);
}

void home_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "=== HOME btn pressed ===");
    if (!home_screen)
    {
        home_screen = lv_obj_create(NULL);

        // 灯光控制按钮
        lv_obj_t *light_btn = lv_button_create(home_screen);
        lv_obj_set_size(light_btn, 120, 60);
        lv_obj_align(light_btn, LV_ALIGN_CENTER, 0, -50);
        lv_obj_t *lbl = lv_label_create(light_btn);
        lv_label_set_text_static(lbl, LV_SYMBOL_POWER " LIGHT");
        lv_obj_set_style_bg_color(light_btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(light_btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
        lv_obj_add_event_cb(light_btn, light_btn_cb, LV_EVENT_PRESSED, NULL);

        // 返回主界面按钮
        lv_obj_t *ret_btn = lv_button_create(home_screen);
        lv_obj_set_size(ret_btn, 120, 60);
        lv_obj_align(ret_btn, LV_ALIGN_CENTER, 0, 50);
        lbl = lv_label_create(ret_btn);
        lv_label_set_text_static(lbl, LV_SYMBOL_HOME " RETURN");
        lv_obj_add_event_cb(ret_btn, ret_cb, LV_EVENT_PRESSED, NULL);
    }
    lv_scr_load(home_screen);
}

// ==================== 主界面 UI ====================

void lvgl_demo_ui(lv_display_t *disp)
{
    main_screen = lv_display_get_screen_active(disp);

    // ===== 时间（占屏幕上 1/3，锁屏风格大字）=====
    static lv_style_t style_time;
    lv_style_init(&style_time);
    lv_style_set_text_font(&style_time, &lv_font_montserrat_48);
    lv_style_set_text_color(&style_time, lv_color_black());

    time_label = lv_label_create(main_screen);
    lv_label_set_text_static(time_label, "00:00");
    lv_obj_add_style(time_label, &style_time, 0);
    lv_obj_set_size(time_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 40);

    // 每秒刷新时间的定时器
    lv_timer_create(time_update_cb, 1000, NULL);

    // ===== 家居按钮 =====
    lv_obj_t *home_btn = lv_button_create(main_screen);
    lv_obj_set_size(home_btn, 160, 70);
    lv_obj_align(home_btn, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_style_bg_color(home_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
    lv_obj_t *home_lbl = lv_label_create(home_btn);
    lv_label_set_text_static(home_lbl, LV_SYMBOL_HOME " JIAJU");
    lv_obj_add_event_cb(home_btn, home_cb, LV_EVENT_PRESSED, NULL);

    // ===== 退出按钮（与家居按钮等大等距）=====
    lv_obj_t *exit_btn = lv_button_create(main_screen);
    lv_obj_set_size(exit_btn, 160, 70);
    lv_obj_align(exit_btn, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(exit_btn, lv_palette_main(LV_PALETTE_RED), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(exit_btn, lv_palette_main(LV_PALETTE_RED), LV_STATE_PRESSED);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_label_set_text_static(exit_lbl, LV_SYMBOL_CLOSE " EXIT");
    lv_obj_add_event_cb(exit_btn, exit_cb, LV_EVENT_PRESSED, NULL);
}

// LVGL 时钟滴答

void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

//  LVGL 主循环任务
void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1)
    {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);

        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

// 触摸读取回调

void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_point_data_t tp_data[1] = {0};
    uint8_t tp_cnt = 0;

    esp_lcd_touch_handle_t touch_pad = lv_indev_get_user_data(indev);
    esp_lcd_touch_read_data(touch_pad);
    esp_lcd_touch_get_data(touch_pad, tp_data, &tp_cnt, 1);

    if (tp_cnt > 0)
    {
        data->point.x = tp_data[0].x;
        data->point.y = tp_data[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// LVGL 总入口

void lvgl_start(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io)
{
    _lock_init(&lvgl_api_lock);

    // NTP 时间同步（需 WiFi 已连接）
    lvgl_ntp_init();

    // 初始化 LVGL 内部状态
    lv_init();

    // 创建显示设备
    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);

    // 分配双缓冲（DMA 安全内存）
    size_t draw_buffer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    lv_display_set_buffers(lvgl_disp, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(lvgl_disp, panel);
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);

    // 注册 SPI 颜色传输完成回调（DMA 完成后通知 LVGL）
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_update_callback,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, lvgl_disp));

    // 创建 LVGL 时钟滴答定时器（2ms）
    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // 触摸屏初始化（如果启用）
#if LCD_TOUCH_ENABLED
    esp_lcd_touch_handle_t tp = lcd_touch_init();
    if (tp)
    {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, lvgl_touch_cb);
        lv_indev_set_user_data(indev, tp);
    }
#endif

    // 创建演示 UI（此时 LVGL 任务尚未启动，单线程安全）
    lvgl_demo_ui(lvgl_disp);

    // 启动 LVGL 主循环任务
    xTaskCreate(lvgl_port_task, "lvgl_task", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "LVGL start success");
}
