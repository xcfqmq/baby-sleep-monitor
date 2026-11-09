#ifndef MQTT_H_
#define MQTT_H_

#include <stdbool.h>

/*
 * 初始化 MQTT 客户端并启动（broker 地址见 mqtt.c 顶部的 MQTT_BROKER_URI）。
 * esp-mqtt 默认会自动重连，这里只是缩短了重连间隔并记录状态。
 */
void MQTT_init(void);

/* 当前是否已连接到 broker */
bool MQTT_is_connected(void);

/*
 * 发布一条消息（QoS 0，不保留）。
 * @return 消息 id；失败或未连接时返回 -1
 */
int MQTT_publish(const char *topic, const char *payload);

#endif
