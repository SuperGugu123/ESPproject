#ifndef __SG90_H_
#define __SG90_H_

/* SG90 舵机控制模块头文件 */

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_log.h"

/* SG90 PWM 控制引脚 */
#define SG90_PWM_PIN GPIO_NUM_1
/* 距离阈值，单位为毫米；超过该距离时打开舵机 */
#define SG90_DISTANCE_THRESHOLD_MM 200
/* 舵机最小脉宽，单位为微秒 */
#define SG90_MIN_PULSE_US 500
/* 舵机最大脉宽，单位为微秒 */
#define SG90_MAX_PULSE_US 2500
/* PWM 周期，单位为微秒，对应 50Hz */
#define SG90_PERIOD_US 20000
/* 默认打开角度 */
#define SG90_DEFAULT_OPEN_ANGLE 120
/* 初始化 SG90 舵机对应的 PWM 和互斥锁资源 */
void sg90_init(void);

/* 设置舵机角度 */
esp_err_t sg90_set_angle(uint32_t angle);

/* 直接将角度转换为 PWM 占空比并输出到舵机 */
esp_err_t sg90_apply_angle(uint32_t angle);

/* 获取当前记录的舵机角度 */
uint32_t sg90_get_angle(void);

/* 根据测得距离控制舵机开合角度 */
void sg90_angle(uint32_t sr04_distance);

#endif
