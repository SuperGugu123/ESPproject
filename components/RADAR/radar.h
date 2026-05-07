#ifndef __RADAR_H_
#define __RADAR_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_radar.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

// NVS 命名空间和键名
// 这里把训练得到的原始阈值持久化，方便重启后直接恢复
#define RADAR_NVS_NAMESPACE "radar_cfg"
#define RADAR_NVS_KEY "thresholds"
#define RADAR_TRAIN_DATA_KEY "train_data"

// 用固定魔数标记“这块 NVS 数据确实是我们的雷达阈值”
#define RADAR_THRESHOLD_MAGIC 0x52414452U

// 阈值缩放
#define RADAR_PRESENCE_SCALE 0.1f
#define RADAR_MOTION_SCALE 0.45f

// 为了减少状态来回跳，退出阈值比进入阈值更低一些，形成滞回。
#define RADAR_PRESENCE_EXIT_SCALE 0.35f
#define RADAR_MOTION_EXIT_SCALE 0.60f
#define RADAR_EMPTY_CONFIRM_FRAMES 10
#define RADAR_WANDER_FALLBACK_VALUE 0.14f
#define RADAR_PRESENCE_JITTER_FALLBACK_MIN 0.05f
// 是否开启开机训练
#define RADAR_FORCE_TRAIN_ON_BOOT 1

// 候选状态连续出现多少次，才真正切换稳定状态。
#define RADAR_STATE_CONFIRM_FRAMES 5

typedef struct
{
    uint32_t magic;
    uint32_t version;
    float wander_threshold;
    float jitter_threshold;
    float no_person_wander_baseline;
    float static_jitter_baseline;
} radar_threshold_store_t;

typedef enum
{
    RADAR_STATE_EMPTY = 0,
    RADAR_STATE_PRESENCE,
    RADAR_STATE_MOTION,
} radar_state_t;

// 把内部状态枚举值转换成可打印的字符串
const char *radar_state_to_string(radar_state_t state);

// 把训练得到的原始阈值转换成当前业务判定真正使用的阈值
float radar_get_effective_threshold(float raw_threshold, float scale);

// 把状态机恢复到初始状态
// 会清空当前稳定状态、候选状态和连续命中计数
void radar_reset_state_machine(void);

// 把当前训练得到的原始阈值保存到 NVS
esp_err_t radar_save_thresholds_to_nvs(void);

// 从 NVS 读取上一次保存的训练阈值
// 读取成功后会更新全局阈值和状态机的可用标志
esp_err_t radar_load_thresholds_from_nvs(void);

// 根据当前一帧的 wander / jitter 和进入、退出阈值
// 先给出“这一帧想要切到什么状态”的结果
// 这里只给出期望状态，真正是否切换还要交给防抖逻辑判断
radar_state_t radar_get_desired_state(float wander,
                                      float jitter,
                                      float presence_enter_threshold,
                                      float presence_exit_threshold,
                                      float motion_enter_threshold,
                                      float motion_exit_threshold);

// 根据期望状态更新稳定状态。
// 只有同一个期望状态连续出现足够多次，才会真正切换，避免状态来回跳。
void radar_update_stable_state(radar_state_t desired_state);

// esp-radar 的结果回调函数。
void radar_rx_cb(void *ctx, const wifi_radar_info_t *info);

// 初始化雷达模块。
esp_err_t radar_init(void);

// 执行一次空场训练。
esp_err_t radar_train(uint32_t train_ms);

// 查询当前是否已经有可用阈值。
bool radar_thresholds_ready(void);

radar_state_t radar_get_state(void);
float radar_get_no_person_wander_baseline(void);
float radar_get_static_jitter_baseline(void);

#endif
