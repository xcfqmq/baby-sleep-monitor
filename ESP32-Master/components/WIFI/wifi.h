#ifndef WIFI_H_
#define WIFI_H_

#include <stdio.h>
#include <stdbool.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

/**
 * @brief  初始化 WiFi STA
 *
 * 做四件事：创建 netif / 注册事件回调 / 配置并启动 WiFi / 准备自动重连。
 * 调用后无需再管连接，断线会由内部定时器自动重连。
 */
void WIFI_INIT(void);

/**
 * @brief  阻塞等待「拿到 IP」
 *
 * @param  timeout_ms  超时时间(毫秒)，传 0 表示一直等
 * @return true=已拿到 IP，false=超时（后台仍会继续自动重连）
 */
bool WIFI_wait_ip(uint32_t timeout_ms);

/**
 * @brief  当前是否已连接且已拿到 IP
 */
bool WIFI_is_connected(void);

/**
 * @brief  取当前 IP 字符串；未拿到 IP 时返回 "0.0.0.0"
 *
 * @note   返回值指向内部静态缓冲区，只适合在同一个任务里使用/打印
 */
const char *WIFI_get_ip_str(void);

#endif
