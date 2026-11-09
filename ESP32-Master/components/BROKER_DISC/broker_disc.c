/*
 * broker_disc.c —— 被动监听 MQTT broker 的局域网广播通告
 *
 * 数据流：
 *   PC: broker_responder.py 每 2 秒 → 广播 "MQTT_BROKER <IP>:<PORT>" → 255.255.255.255:8888
 *   ESP32: bind UDP 8888 → recvfrom → 解析 → 拿到 broker 地址
 */
#include "broker_disc.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "DISC";

esp_err_t broker_disc_find(char *ip_buf, size_t ip_buf_size, uint16_t *port_out, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        D_LOGE(TAG, "socket 创建失败");
        return ESP_FAIL;
    }

    /* 绑定本机 UDP 8888 接收广播通告 */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(BROKER_DISC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        D_LOGE(TAG, "bind UDP %d 失败", BROKER_DISC_PORT);
        close(sock);
        return ESP_FAIL;
    }

    /* 每次 recvfrom 最多等 1 秒 */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t   deadline = esp_timer_get_time() / 1000 + timeout_ms;
    char      buf[64];
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    while (esp_timer_get_time() / 1000 < deadline) {
        struct sockaddr_in from;
        socklen_t          fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            continue;                      /* 1 秒内没收到通告，继续等 */
        }
        buf[n] = '\0';

        /* 期望格式: "MQTT_BROKER 192.168.1.101:1883" */
        char         ip[32] = {0};
        unsigned int port   = 0;
        if (sscanf(buf, "MQTT_BROKER %31[^:]:%u", ip, &port) == 2 && port > 0) {
            strncpy(ip_buf, ip, ip_buf_size - 1);
            ip_buf[ip_buf_size - 1] = '\0';
            *port_out = (uint16_t)port;
            D_LOGI(TAG, "收到 broker 通告: %s:%u", ip, (unsigned)port);
            ret = ESP_OK;
            break;
        }
    }

    close(sock);
    return ret;
}
