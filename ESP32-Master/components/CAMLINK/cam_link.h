/*
 * cam_link.h —— 主机侧：通过 I2C 读取摄像头从机（ESP32-S3-Cam, 0x52）的结果
 *
 * 硬件连接（主机 ESP32-Master ⟷ 从机 ESP32-S3-Cam）：
 *   主机 GPIO17 (SDA) ⟷ 从机 IO47 (SDA)
 *   主机 GPIO18 (SCL) ⟷ 从机 IO48 (SCL)
 *   主机 GND         ⟷ 从机 GND
 *
 * 说明：
 *   - 用第二条 I2C 总线（BMI270 占着 GPIO4/5，这里避开）
 *   - .i2c_port = -1 由驱动自动挑一个空闲控制器（BMI270 已经占了另一个）
 *   - 100kHz，和从机侧配置一致
 *
 * 从机寄存器表（字节偏移，详见从机 report.h）：
 *   0   4  标识: 0xA5, 版本, 状态, 保留
 *   4   8  检测框: int16 x, y, w, h
 *   12  4  环境: uint16 亮度, uint16 运动量
 *   16  4  心跳: uint32 帧计数
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_LINK_SDA_GPIO  17      /* 主机侧 SDA */
#define CAM_LINK_SCL_GPIO  18      /* 主机侧 SCL */
#define CAM_LINK_ADDR      0x52    /* 从机地址（厂商同款） */
#define CAM_LINK_FREQ_HZ   100000  /* 100kHz */
#define CAM_LINK_MAGIC     0xA5
#define CAM_LINK_REG_SIZE  20
#define CAM_LINK_REG_CHECKSUM 3    /* 校验和字节偏移（其余 19 字节累加，识别撕裂读用） */

/* 从机状态（一次读回的全部内容） */
typedef struct {
    uint8_t  magic;         /* 0xA5 = 从机在线上 */
    uint8_t  fw_version;
    uint8_t  status;        /* 0=启动中, 1=运行中；bit1=遮挡 */
    bool     occluded;      /* 画面被遮挡（状态字节 bit1） */
    int16_t  x, y, w, h;    /* 运动区域包围盒（图像坐标） */
    uint16_t brightness;    /* 0~4095 */
    uint16_t motion;        /* 0~4095 */
    uint32_t frame_count;   /* 心跳：从机帧计数 */
} cam_state_t;

/* 初始化 I2C 主机总线并挂上从机设备；返回 ESP_OK 表示总线建好了（还没证明从机在线） */
esp_err_t cam_link_init(void);

/* 读一次从机寄存器表。返回 ESP_OK 且 out->magic == 0xA5 才说明从机在线。 */
esp_err_t cam_link_read(cam_state_t *out);

/* 起一个后台任务，每 interval_ms 读一次并打印日志（同时缓存到 cam_link_get_latest） */
void cam_link_start_task(uint32_t interval_ms);

/* 取最近一次成功读到的数据（给 MQTT 上报用）；返回 false 表示还没读到过 */
bool cam_link_get_latest(cam_state_t *out);

#ifdef __cplusplus
}
#endif
