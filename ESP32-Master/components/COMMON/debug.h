/*
 * debug.h —— 全局调试开关（条件编译）
 *
 * 用法：
 *   - 保留下面这行 #define DEBUG  → 正常打印串口日志（调试用）
 *   - 注释掉下面这行              → 关闭所有串口打印（release 模式）
 *
 * 为什么要关：
 *   串口日志没人读时，UART 发送缓冲会填满，反过来阻塞任务（表现为卡顿）。
 *   release 模式只留 OLED + MQTT，不再往串口吐日志。
 */
#pragma once

#include "esp_log.h"

/* ============ 调试开关：注释掉这行 = 关闭串口打印 ============ */
#define DEBUG

#ifdef DEBUG
  #define D_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
  #define D_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
  #define D_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
  #define D_LOGI(tag, fmt, ...) ((void)0)
  #define D_LOGW(tag, fmt, ...) ((void)0)
  #define D_LOGE(tag, fmt, ...) ((void)0)
#endif
