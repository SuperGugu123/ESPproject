#include "mylvgl.h"

static const char *TAG = "LVGL";

// ===== 互斥锁（LVGL 非线程安全，需保护） =====
static _lock_t lvgl_api_lock;

// ===== 全局对象 =====
static lv_display_t *lvgl_disp = NULL;                      // 显示设备
static lv_obj_t *btn;                                       // 旋转按钮
static lv_display_rotation_t rotation = LV_DISP_ROTATION_0; // 当前旋转状态

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

// ==================== 按钮回调：每次点击旋转 90° ====================

void btn_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_user_data(e);
    rotation++;
    if (rotation > LV_DISP_ROTATION_270)
    {
        rotation = LV_DISP_ROTATION_0;
    }
    lv_disp_set_rotation(disp, rotation);
}

// 弧形动画回调

void set_lvgl_angle(void *obj, int32_t v)
{
    lv_arc_set_value(obj, v);
}

// ==================== 演示 UI ====================

void lvgl_demo_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);

    // --- 按钮：点击旋转屏幕 ---
    btn = lv_button_create(scr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_static(lbl, LV_SYMBOL_REFRESH " ROTATE");
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 30, -30);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, disp);

    // --- 弧形进度条（动画演示） ---
    lv_obj_t *arc = lv_arc_create(scr);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);   // 隐藏旋钮，只显示进度
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE); // 禁止手动点击调节
    lv_obj_center(arc);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, set_lvgl_angle);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 500);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_start(&a);
}

// ==================== LVGL 时钟滴答 ====================

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
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;

    esp_lcd_touch_handle_t touch_pad = lv_indev_get_user_data(indev);
    esp_lcd_touch_read_data(touch_pad);
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(touch_pad, touchpad_x, touchpad_y,
                                                          NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0)
    {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
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
