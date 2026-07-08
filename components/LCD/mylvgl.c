#include "mylvgl.h"
#include "esp_app_desc.h"
#include "wifista.h"
#include "esp_sleep.h"

static const char *TAG = "LVGL";

// ===== 互斥锁（LVGL 非线程安全，需保护） =====
static _lock_t lvgl_api_lock;

// ===== 同步事件组（WiFi连接后通知 main） =====
EventGroupHandle_t g_wifi_sync_group = NULL;

// ===== 全局对象 =====
static lv_display_t *lvgl_disp = NULL; // 显示设备
static lv_obj_t *main_screen = NULL;   // 主界面
static lv_obj_t *home_screen = NULL;   // 家居界面
static lv_obj_t *time_label = NULL;    // 时间标签
static bool ntp_synced = false;        // NTP 是否已同步

// ===== WiFi 配置界面对象 =====
static lv_obj_t *wifi_screen = NULL;
static lv_obj_t *ssid_ta = NULL;
static lv_obj_t *pwd_ta = NULL;
static lv_obj_t *kb = NULL;
static lv_obj_t *status_box = NULL;
static lv_obj_t *connect_btn = NULL;
static lv_obj_t *connect_btn_label = NULL;
static lv_obj_t *status_box_label = NULL;
static lv_timer_t *wifi_check_timer = NULL;
static int wifi_check_count = 0;
static bool wifi_is_reconnect = false;
static bool wifi_auto_connect = false;
static lv_obj_t *wifi_prev_screen = NULL;

#define WIFI_TIMEOUT_SEC 15

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

// ==================== WiFi 配置界面 ====================

static void kb_ready_cb(lv_event_t *e)
{
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// 前置声明（函数间互相引用）
static void wifi_status_check_cb(lv_timer_t *timer);
static void wifi_connect_btn_cb(lv_event_t *e);
static void wifi_cancel_btn_cb(lv_event_t *e);
static void wifi_back_btn_cb(lv_event_t *e);
static void timeout_box_close_cb(lv_event_t *e);

// 取消连接
static void wifi_cancel_connect(void)
{
    if (wifi_check_timer)
    {
        lv_timer_del(wifi_check_timer);
        wifi_check_timer = NULL;
    }
    lv_obj_remove_state(ssid_ta, LV_STATE_DISABLED);
    lv_obj_remove_state(pwd_ta, LV_STATE_DISABLED);
    lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
    lv_obj_add_flag(status_box, LV_OBJ_FLAG_HIDDEN);

    // 恢复按钮为 Connect
    lv_label_set_text(connect_btn_label, "Connect");
    lv_obj_set_style_bg_color(connect_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
    lv_obj_remove_event_cb(connect_btn, wifi_cancel_btn_cb);
    lv_obj_add_event_cb(connect_btn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);

    wifi_auto_connect = false;
    ESP_LOGI(TAG, "WiFi connect cancelled");
}

// 自动连接超时弹窗关闭回调：清空 SSID/密码让用户重新输入
static void timeout_box_close_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_obj_t *msgbox = lv_obj_get_parent(btn);
    lv_obj_del(msgbox);

    lv_textarea_set_text(ssid_ta, "");
    lv_textarea_set_text(pwd_ta, "");
    ESP_LOGI(TAG, "Auto-connect timeout, fields cleared");
}

// 取消按钮回调
static void wifi_cancel_btn_cb(lv_event_t *e)
{
    wifi_cancel_connect();
}

// 返回按钮回调：取消连接并返回上一级界面
static void wifi_back_btn_cb(lv_event_t *e)
{
    if (wifi_check_timer)
    {
        lv_timer_del(wifi_check_timer);
        wifi_check_timer = NULL;
    }
    wifi_auto_connect = false;

    lv_obj_del(wifi_screen);
    wifi_screen = NULL;

    if (wifi_prev_screen)
    {
        lv_scr_load(wifi_prev_screen);
        wifi_prev_screen = NULL;
    }
    wifi_is_reconnect = false;
    ESP_LOGI(TAG, "WiFi setup back to previous screen");
}

// 执行 WiFi 连接
static void do_wifi_connect(void)
{
    const char *ssid = lv_textarea_get_text(ssid_ta);
    const char *pwd = lv_textarea_get_text(pwd_ta);
    if (ssid[0] == '\0')
        return;

    lv_obj_add_state(ssid_ta, LV_STATE_DISABLED);
    lv_obj_add_state(pwd_ta, LV_STATE_DISABLED);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // 按钮切换为 Cancel
    lv_label_set_text(connect_btn_label, "Cancel");
    lv_obj_set_style_bg_color(connect_btn, lv_palette_main(LV_PALETTE_RED), LV_STATE_DEFAULT);
    lv_obj_remove_event_cb(connect_btn, wifi_connect_btn_cb);
    lv_obj_add_event_cb(connect_btn, wifi_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);

    // 显示状态框
    lv_label_set_text(status_box_label, "Connecting...");
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_HIDDEN);

    wifi_connect_to_ap(ssid, pwd);
    wifi_check_count = 0;
    wifi_check_timer = lv_timer_create(wifi_status_check_cb, 500, NULL);
    ESP_LOGI(TAG, "WiFi connecting to %s ...", ssid);
}

// WiFi 连接状态检查（500ms 一次）
static void wifi_status_check_cb(lv_timer_t *timer)
{
    wifi_check_count++;

    if (wifi_is_connected())
    {
        const char *ssid = lv_textarea_get_text(ssid_ta);
        const char *pwd = lv_textarea_get_text(pwd_ta);
        wifi_save_credentials(ssid, pwd);

        if (wifi_is_reconnect)
        {
            lv_obj_del(wifi_screen);
            wifi_screen = NULL;
            if (wifi_prev_screen)
            {
                lv_scr_load(wifi_prev_screen);
                wifi_prev_screen = NULL;
            }
            wifi_is_reconnect = false;
        }
        else
        {
            if (g_wifi_sync_group)
            {
                xEventGroupSetBits(g_wifi_sync_group, WIFI_CONFIG_DONE_BIT);
            }
            lvgl_ntp_init();

            lv_obj_t *new_screen = lv_obj_create(NULL);
            lv_scr_load(new_screen);
            lv_obj_del(wifi_screen);
            wifi_screen = NULL;

            lvgl_demo_ui(lvgl_disp);
        }

        lv_timer_del(timer);
        wifi_check_timer = NULL;
        ESP_LOGI(TAG, "WiFi connected, credentials saved");
    }
    else if (wifi_check_count >= (WIFI_TIMEOUT_SEC * 2))
    {
        // 15 秒超时
        lv_label_set_text(status_box_label, "Timeout, retry...");
        ESP_LOGW(TAG, "WiFi connection timeout");

        if (wifi_auto_connect)
        {
            // 自动连接超时：弹出提示框
            wifi_auto_connect = false;
            vTaskDelay(pdMS_TO_TICKS(800));
            wifi_cancel_connect();

            lv_obj_t *msgbox = lv_obj_create(wifi_screen);
            lv_obj_set_size(msgbox, 240, 120);
            lv_obj_center(msgbox);
            lv_obj_set_style_bg_color(msgbox, lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_width(msgbox, 2, 0);
            lv_obj_set_style_border_color(msgbox, lv_palette_main(LV_PALETTE_RED), 0);

            lv_obj_t *msg_label = lv_label_create(msgbox);
            lv_label_set_text(msg_label, "Connection timeout!\nPlease re-enter WiFi info.");
            lv_obj_set_style_text_color(msg_label, lv_color_white(), 0);
            lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(msg_label, LV_ALIGN_TOP_MID, 0, 20);

            lv_obj_t *ok_btn = lv_button_create(msgbox);
            lv_obj_set_size(ok_btn, 80, 35);
            lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, -15);
            lv_obj_set_style_bg_color(ok_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
            lv_obj_t *ok_label = lv_label_create(ok_btn);
            lv_label_set_text(ok_label, "OK");
            lv_obj_center(ok_label);
            lv_obj_add_event_cb(ok_btn, timeout_box_close_cb, LV_EVENT_CLICKED, NULL);
        }
        else
        {
            // 手动连接超时：恢复到可编辑状态
            vTaskDelay(pdMS_TO_TICKS(800));
            wifi_cancel_connect();
        }
    }
}

// 连接按钮回调
static void wifi_connect_btn_cb(lv_event_t *e)
{
    do_wifi_connect();
}

// 创建 WiFi 配置界面（首次启动和切换 WiFi 共用）
static void create_wifi_config_screen(void)
{
    wifi_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_screen, lv_color_white(), 0);

    // 标题
    lv_obj_t *title = lv_label_create(wifi_screen);
    lv_label_set_text(title, "WiFi Setup");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // SSID
    ssid_ta = lv_textarea_create(wifi_screen);
    lv_textarea_set_one_line(ssid_ta, true);
    lv_textarea_set_placeholder_text(ssid_ta, "WiFi Name");
    lv_obj_set_size(ssid_ta, 200, 40);
    lv_obj_align(ssid_ta, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_add_event_cb(ssid_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // 密码
    pwd_ta = lv_textarea_create(wifi_screen);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_textarea_set_password_mode(pwd_ta, true);
    lv_textarea_set_placeholder_text(pwd_ta, "Password");
    lv_obj_set_size(pwd_ta, 200, 40);
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_event_cb(pwd_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    // 预填 NVS 中保存的凭证
    char saved_ssid[33] = {0};
    char saved_pwd[65] = {0};
    bool has_credentials = (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_pwd, sizeof(saved_pwd)) == ESP_OK);
    if (has_credentials)
    {
        lv_textarea_set_text(ssid_ta, saved_ssid);
        lv_textarea_set_text(pwd_ta, saved_pwd);
    }

    // Connect 按钮
    connect_btn = lv_button_create(wifi_screen);
    lv_obj_set_size(connect_btn, 160, 50);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_MID, 0, 185);
    lv_obj_set_style_bg_color(connect_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
    connect_btn_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_btn_label, "Connect");
    lv_obj_add_event_cb(connect_btn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);

    // Return 按钮（仅从 home 界面进入时显示，放在 Connect 下方）
    if (wifi_is_reconnect)
    {
        lv_obj_t *return_btn = lv_button_create(wifi_screen);
        lv_obj_set_size(return_btn, 160, 45);
        lv_obj_align(return_btn, LV_ALIGN_TOP_MID, 0, 245);
        lv_obj_set_style_bg_color(return_btn, lv_color_hex(0x999999), LV_STATE_DEFAULT);
        lv_obj_t *return_lbl = lv_label_create(return_btn);
        lv_label_set_text(return_lbl, "Return");
        lv_obj_center(return_lbl);
        lv_obj_add_event_cb(return_btn, wifi_back_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    // 状态提示框（默认隐藏）
    status_box = lv_obj_create(wifi_screen);
    lv_obj_set_size(status_box, 200, 60);
    lv_obj_align(status_box, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(status_box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(status_box, 0, 0);
    status_box_label = lv_label_create(status_box);
    lv_label_set_text(status_box_label, "Connecting...");
    lv_obj_set_style_text_color(status_box_label, lv_color_white(), 0);
    lv_obj_center(status_box_label);
    lv_obj_add_flag(status_box, LV_OBJ_FLAG_HIDDEN);

    // 键盘
    kb = lv_keyboard_create(wifi_screen);
    lv_obj_set_size(kb, 240, 120);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_ready_cb, LV_EVENT_READY, NULL);

    lv_scr_load(wifi_screen);

    // 如果 NVS 中有凭证且不是切换 WiFi 场景，自动连接
    if (has_credentials && !wifi_is_reconnect)
    {
        wifi_auto_connect = true;
        do_wifi_connect();
    }

    ESP_LOGI(TAG, "WiFi config screen created");
}

// 切换 WiFi（home 界面调用）
void lvgl_show_change_wifi(void)
{
    wifi_is_reconnect = true;
    wifi_prev_screen = lv_scr_act();
    create_wifi_config_screen();
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
    ESP_LOGW(TAG, "=== SHUTDOWN btn pressed ===");
    lv_obj_clean(lv_scr_act());
    esp_lcd_panel_disp_on_off(lcd_panel_handle(), false);
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_deep_sleep_start();
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

void change_wifi_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "=== CHANGE WIFI btn pressed ===");
    lvgl_show_change_wifi();
}

void home_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "=== HOME btn pressed ===");
    if (!home_screen)
    {
        home_screen = lv_obj_create(NULL);

        // 灯光控制按钮
        lv_obj_t *light_btn = lv_button_create(home_screen);
        lv_obj_set_size(light_btn, 120, 50);
        lv_obj_align(light_btn, LV_ALIGN_CENTER, 0, -70);
        lv_obj_t *lbl = lv_label_create(light_btn);
        lv_label_set_text_static(lbl, LV_SYMBOL_POWER " LIGHT");
        lv_obj_set_style_bg_color(light_btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(light_btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
        lv_obj_add_event_cb(light_btn, light_btn_cb, LV_EVENT_PRESSED, NULL);

        // 切换 WiFi 按钮
        lv_obj_t *chwifi_btn = lv_button_create(home_screen);
        lv_obj_set_size(chwifi_btn, 120, 50);
        lv_obj_align(chwifi_btn, LV_ALIGN_CENTER, 0, 0);
        lbl = lv_label_create(chwifi_btn);
        lv_label_set_text_static(lbl, LV_SYMBOL_WIFI " WIFI");
        lv_obj_add_event_cb(chwifi_btn, change_wifi_cb, LV_EVENT_PRESSED, NULL);

        // 返回主界面按钮
        lv_obj_t *ret_btn = lv_button_create(home_screen);
        lv_obj_set_size(ret_btn, 120, 50);
        lv_obj_align(ret_btn, LV_ALIGN_CENTER, 0, 70);
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
    lv_label_set_text_static(home_lbl, LV_SYMBOL_HOME " HOME");
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

    // ===== 版本号（底部小字）=====
    const esp_app_desc_t *desc = esp_app_get_description();
    char ver_buf[32];
    snprintf(ver_buf, sizeof(ver_buf), "version: %s", desc->version);

    lv_obj_t *ver_label = lv_label_create(main_screen);
    lv_label_set_text(ver_label, ver_buf);
    lv_obj_set_style_text_font(ver_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ver_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(ver_label, LV_ALIGN_BOTTOM_MID, 0, -5);
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
        // 180° 旋转后触摸坐标映射（Y 翻转，X 保持）
        data->point.x = tp_data[0].x;
        data->point.y = LCD_V_RES - 1 - tp_data[0].y;
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

    // 创建同步事件组（WiFi 连接后用于通知 main）
    g_wifi_sync_group = xEventGroupCreate();

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

    // 创建 WiFi 配置界面（LVGL 任务启动后显示）
    create_wifi_config_screen();

    // 启动 LVGL 主循环任务
    xTaskCreate(lvgl_port_task, "lvgl_task", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "LVGL start success");
}
