/*
 * report.h —— 从机上报组件：把检测结果通过 I2C 从机（0x52）暴露给主机
 *
 * 通信方式：ESP32-S3 的 I2C 从机「RAM 寄存器模式」
 *   - 从机把一张寄存器表放进 I2C 外设的 RAM（硬件维护）
 *   - 主机用标准的「写寄存器偏移 → 读数据」取值，和读 BMI270 寄存器一模一样
 *   - 地址 0x52 沿用厂商例程，主机侧代码可以完全照搬
 *
 * 寄存器表（小端）：
 *   偏移  长度  内容
 *   0x00   4    设备标识：[0]=0xA5 魔术字, [1]=固件版本, [2]=状态, [3]=校验和
 *   0x04   8    检测结果 int16 ×4：x, y, w, h（对齐人脸/人形检测框；无目标时为 0）
 *   0x0C   4    环境量  uint16 ×2：亮度(0~4095), 运动量(0~4095)
 *   0x10   4    心跳    uint32   ：帧计数（主机据此判断从机是否在跑）
 *   共 20 字节（I2C RAM 上限 32 字节）；校验和 = 其余 19 字节累加（自身除外），
 *   主机靠它识别「撕裂读」（从机重写寄存器表时恰好被读到一半）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 接线与地址 ===================== */
#define REPORT_I2C_ADDR      0x52   /* 从机地址（与厂商例程一致） */
#define REPORT_I2C_SDA_GPIO  47     /* 从机侧 SDA 引脚（厂商代码 sdaPin=47） */
#define REPORT_I2C_SCL_GPIO  48     /* 从机侧 SCL 引脚（厂商代码 sclPin=48） */

/* 寄存器表布局（I2C RAM 模式下，主机发的第一个字节就是「字节偏移」，各块必须连续不重叠）：
 *   偏移  长度  内容
 *   0     4    设备标识：[0]=0xA5 魔术字, [1]=固件版本, [2]=状态, [3]=校验和
 *   4     8    检测结果 int16 ×4：x, y, w, h（对齐人脸/人形检测框；无目标时为 0）
 *   12    4    环境量  uint16 ×2：亮度(0~4095), 运动量(0~4095)
 *   16    4    心跳    uint32   ：帧计数（主机据此判断从机是否在跑）
 *   共 20 字节（I2C RAM 上限 32 字节）；校验和 = 其余 19 字节累加（自身除外）
 */
#define REPORT_MAGIC         0xA5
#define REPORT_FW_VERSION    0x01

#define REPORT_REG_ID        0    /* 4 字节 */
#define REPORT_REG_CHECKSUM  3    /* 1 字节：其余 19 字节的累加和 */
#define REPORT_REG_DETECT    4    /* 8 字节 */
#define REPORT_REG_ENV       12   /* 4 字节 */
#define REPORT_REG_HEARTBEAT 16   /* 4 字节 */
#define REPORT_REG_SIZE      20

/* 状态字节取值 */
typedef enum {
    REPORT_STATUS_BOOT = 0,   /* 刚启动，还没抓到帧 */
    REPORT_STATUS_RUN  = 1,   /* 正常抓帧中 */
} report_status_t;

/* 状态字节里的附加标志位 */
#define REPORT_FLAG_OCCLUDED  0x02   /* bit1：画面被遮挡（被子盖住摄像头/脸） */

/* 初始化 I2C 从机（含寄存器表初始值）。返回 ESP_OK 表示主机可以来读了。 */
esp_err_t report_init(void);

/* 把内存里的寄存器表刷进 I2C 硬件 RAM（每次更新数据后调用） */
esp_err_t report_flush(void);

void report_set_status(report_status_t st);
void report_set_occluded(bool occluded);
void report_set_detect(int16_t x, int16_t y, int16_t w, int16_t h);
void report_set_env(uint16_t brightness, uint16_t motion);
void report_set_frame_count(uint32_t count);

#ifdef __cplusplus
}
#endif
