#ifndef _MYLVGL_H
#define _MYLVGL_H

#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "lcd.h"

//  LVGL 参数
#define LVGL_DRAW_BUF_LINES 20                             // 绘制缓冲覆盖的行数（建议 ≥ 屏幕高度的 1/10）
#define LVGL_TICK_PERIOD_MS 2                              // LVGL 时钟滴答间隔（毫秒）
#define LVGL_TASK_MAX_DELAY_MS 500                         // LVGL 任务最大休眠时间（防卡死）
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ) // LVGL 任务最小休眠（防看门狗）
#define LVGL_TASK_STACK_SIZE (4 * 1024)                    // LVGL 任务栈大小
#define LVGL_TASK_PRIORITY 2                               // LVGL 任务 FreeRTOS 优先级

//  LVGL 总入口
void lvgl_start(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io);

// 演示 UI
void lvgl_demo_ui(lv_display_t *disp);

// ===== 内部回调（FreeRTOS 任务/定时器需要） =====
void btn_cb(lv_event_t *e);
void set_lvgl_angle(void *obj, int32_t v);
void increase_lvgl_tick(void *arg);
void lvgl_port_task(void *arg);
void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data);

#endif // _MYLVGL_H
