#ifndef LCD_H
#define LCD_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_xpt2046.h"

// 使用 SPI2 主机
#define LCD_HOST SPI2_HOST

//  SPI 时钟
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000) // SPI 时钟 20MHz（ILI9341 最高约 40MHz）

//  背光控制
#define LCD_BK_LIGHT_ON_LEVEL 1                       // 背光点亮时的 GPIO 电平（高电平有效）
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL // 背光关闭

//  引脚定义
#define PIN_NUM_SCLK 13     // SPI 时钟线 SCLK
#define PIN_NUM_MOSI 12     // SPI MOSI（主机发数据给屏幕）
#define PIN_NUM_MISO 5      // SPI MISO（屏幕回传数据，可设 -1 不用）
#define PIN_NUM_LCD_DC 11   // 数据/命令选择线 D/C#（0=命令, 1=数据）
#define PIN_NUM_LCD_RST 10  // LCD 硬件复位引脚（低电平复位）
#define PIN_NUM_LCD_CS 9    // SPI 片选 CS（选中 LCD）
#define PIN_NUM_BK_LIGHT 6  // 背光控制 GPIO
#define PIN_NUM_TOUCH_CS 14 // 触摸屏 SPI 片选（共享同一 SPI 总线）
#define PIN_NUM_TOUCH_IRQ 4 // 触摸屏中断检测引脚

//  分辨率
#define LCD_H_RES 240 // 水平方向像素数（X 轴）
#define LCD_V_RES 320 // 垂直方向像素数（Y 轴）

//  SPI 命令/参数位宽
#define LCD_CMD_BITS 8   // SPI 发送命令时用 8 位
#define LCD_PARAM_BITS 8 // SPI 发送参数时用 8 位

// 触摸使能开关
#define LCD_TOUCH_ENABLED 1

// LCD 硬件初始化（纯硬件，屏幕点亮即结束）
esp_err_t lcd_init(void);

//  获取面板/IO句柄
esp_lcd_panel_handle_t lcd_panel_handle(void);
esp_lcd_panel_io_handle_t lcd_io_handle(void);

esp_lcd_touch_handle_t lcd_touch_handle(void);
esp_lcd_touch_handle_t lcd_touch_init(void);

#endif
