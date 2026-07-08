#ifndef __APP_SR_H_
#define __APP_SR_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "inmp441.h"
#include "model_path.h"
#include "app_mqtt.h"

// AFE 输入通道格式
// 当前硬件只接了一个 INMP441 麦克风，所以使用 "M"
#define APP_SR_INPUT_FORMAT "M"

// MultiNet 命令词检测窗口。
// 这个值会传给 multinet->create()，用于控制一次唤醒后命令词检测的持续时间。
//  这里沿用 ESP-SR 示例常见值 5760。
#define APP_SR_MN_TIMEOUT 5760

/*
 * INMP441 原始采样缩放位移。
 *
 * INMP441 常见输出是 32-bit I2S slot 中放 24-bit 有效音频。
 * AFE 需要 16-bit PCM，所以这里右移 16 位得到 int16_t 音频数据。
 */
#define APP_SR_INMP441_TO_PCM_SHIFT 16

/* 识别结果队列长度。队列里存放 sr_result_t。 */
#define APP_SR_RESULT_QUEUE_LEN 4

/* 采集任务栈大小。该任务负责从 INMP441 读取音频并喂给 AFE。 */
#define APP_SR_FEED_TASK_STACK_SIZE (4 * 1024)

/* 识别任务栈大小。该任务负责唤醒词和命令词识别。 */
#define APP_SR_DETECT_TASK_STACK_SIZE (6 * 1024)

/* 结果处理任务栈大小。该任务负责消费识别结果队列。 */
#define APP_SR_HANDLER_TASK_STACK_SIZE (4 * 1024)

/*
 * ESP-SR 运行时建议的最小 PSRAM 容量。
 *
 * 当前启用的 WakeNet9 + MultiNet7 AC 组合会申请较多连续内存。
 * 如果没有 PSRAM，AFE 创建阶段可能在 ESP-SR 内部因为 ring buffer 分配失败而崩溃。
 */
#define APP_SR_MIN_PSRAM_SIZE (2 * 1024 * 1024)

/*
 * 是否连续识别命令。
 *
 * 0：识别到一个命令后重新打开 WakeNet，下一次命令需要重新唤醒。
 * 1：唤醒后可以连续识别多个命令，直到 MultiNet 超时。
 */
#define SR_CONTINUE_DET 0

/*
 * 兼容旧 ESP-SR 示例里的彩色日志写法。
 *
 * 当前工程使用的 ESP-IDF 日志宏不适合直接拼接 LOG_BOLD(LOG_COLOR_GREEN)，
 * 所以没有 LOG_BOLD 时把它定义为空字符串，保留示例代码结构并保证可编译。
 */
#ifndef LOG_BOLD
#define LOG_BOLD(color) ""
#endif

/*
 * 语音识别结果。
 *
 * wakenet_mode：唤醒词状态，例如 WAKENET_DETECTED 表示检测到唤醒词。
 * state：MultiNet 命令词识别状态，例如 ESP_MN_STATE_DETECTED 或 ESP_MN_STATE_TIMEOUT。
 * command_id：命令编号，对应 app_sr.c 中 s_cmd_phoneme 数组的下标。
 */
typedef struct
{
    wakenet_state_t wakenet_mode;
    esp_mn_state_t state;
    int command_id;
} sr_result_t;

/*
 * 将 INMP441 读到的 32-bit I2S 原始采样转换成 AFE 需要的 16-bit PCM。
 *
 * raw_sample：INMP441 原始采样。
 * 返回值：缩放并限幅后的 int16_t PCM 采样。
 */
int16_t app_sr_convert_inmp441_sample(int32_t raw_sample);

/*
 * 音频采集任务。
 *
 * 从 INMP441 读取音频，转换成 16-bit PCM，并通过 s_afe_handle->feed() 送入 AFE。
 * pvParam 必须传入 esp_afe_sr_data_t *。
 */
void audio_feed_task(void *pvParam);

/*
 * 语音识别任务。
 *
 * 从 AFE fetch 音频前端结果，先处理 WakeNet 唤醒词状态，
 * 唤醒后再调用 MultiNet 做命令词识别，并把结果发送到队列。
 * pvParam 必须传入 esp_afe_sr_data_t *。
 */
void audio_detect_task(void *pvParam);

/*
 * 识别结果处理任务。
 *
 * 从结果队列中取出 sr_result_t 并打印日志。
 * 后续如果要控制灯、舵机或其他外设，可以在这个函数里按 command_id 分发动作。
 * pvParam 必须传入 QueueHandle_t。
 */
void sr_handler_task(void *pvParam);

/*
 * 启动语音识别。
 *
 * 该函数会加载 model 分区中的 ESP-SR 模型，创建 AFE、WakeNet、MultiNet，
 * 注册命令词列表，并创建采集、识别、结果处理三个任务。
 *
 * 调用前需要先完成 inmp441_init()。
 * 如果要命令词识别，sdkconfig 中必须启用中文 MultiNet 模型。
 */
esp_err_t app_sr_start(void);

/*
 * 重置命令词列表。
 *
 * 当前是预留接口，暂未真正实现动态命令词更新。
 * command_list 为空时返回 ESP_ERR_INVALID_ARG，否则返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t app_sr_reset_command_list(char *command_list);

#endif
