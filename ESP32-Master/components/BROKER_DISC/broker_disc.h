/*
 * broker_disc.h —— MQTT broker 自动发现（电脑广播、ESP32 监听）
 *
 * 方向：电脑上的 broker_responder.py 每 2 秒向 255.255.255.255:8888 广播
 *       "MQTT_BROKER <IP>:<PORT>"，ESP32 在这里 bind UDP 8888 被动接收。
 *
 * 为什么不是 ESP32 主动查询？
 *   Windows 防火墙默认拦截"从外部主动进来的" UDP 包。如果 ESP32 广播查询、
 *   电脑回复，那个回复包会在电脑侧被防火墙丢掉（实测就是这样）。
 *   反过来由电脑主动广播、ESP32 被动收，出站包由电脑自己发出，必达。
 *
 * 前提：ESP32 和电脑在同一局域网。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BROKER_DISC_PORT   8888

/*
 * 监听局域网内 MQTT broker 的广播通告。
 *   ip_buf / ip_buf_size : 输出 IP 字符串缓冲区
 *   port_out             : 输出端口
 *   timeout_ms           : 最多等多久（广播每 2 秒一次，建议 ≥6000）
 * 成功返回 ESP_OK；超时返回 ESP_ERR_NOT_FOUND（调用方用备用 IP 兜底）。
 */
esp_err_t broker_disc_find(char *ip_buf, size_t ip_buf_size, uint16_t *port_out, int timeout_ms);

#ifdef __cplusplus
}
#endif
