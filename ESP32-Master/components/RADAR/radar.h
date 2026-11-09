/*
 * radar.h —— HLK-LD6002 呼吸心率雷达（主机侧 UART 接入）
 *
 * 硬件接线（只接了 LD6002，没接 LD6003B）：
 *   雷达 TX0 ──► 主机 GPIO9  (UART1 RX)
 *   雷达 RX0 ◄── 主机 GPIO10 (UART1 TX)
 *   雷达 GND ──  主机 GND（必须共地）
 *   雷达 3V3 ──  独立 3.3V 电源（≥1A，不要接主机 3.3V！）
 *
 * 协议（帧结构同 LD6003B，已用实测数据验证）：
 *   SOF(1)=0x01 | 帧ID(2,大端) | LEN(2,大端) | TYPE(2,大端) | 头校验(1) | DATA(LEN) | 数据校验(1)
 *   整帧长度 = LEN + 9；DATA 里的数值都是 float32 小端。
 *
 * 消息类型（实测 10 种）：
 *   0x0A38 LEN=1  状态
 *   0x0A04 LEN=20 目标 id/x/y/z
 *   0x0A18 LEN=2  状态
 *   0x0A16 LEN=8  flag(4) + 距离 float(cm)     ← flag=1 表示有目标
 *   0x0F09 LEN=2  状态
 *   0x0A17 LEN=8  位置 x/y(float)
 *   0x0A13 LEN=12 相位：总/呼吸/心跳(3×float)  ← 呼吸相位做波形→呼吸暂停检测
 *   0x0A14 LEN=4  呼吸速率(float bpm)
 *   0x0A15 LEN=4  心跳速率(float bpm)
 *   0x0100 LEN=23/24 ASCII 文本（调试输出，忽略）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 接线与串口配置 ===================== */
#define RADAR_UART_NUM   1          /* UART1（UART0 被串口日志占用） */
#define RADAR_PIN_RX     9          /* 主机 RX ← 雷达 TX0 */
#define RADAR_PIN_TX     10         /* 主机 TX → 雷达 RX0 */
#define RADAR_BAUD       115200

/* ===================== 消息类型 ===================== */
#define RADAR_T_STATUS   0x0A38     /* LEN=1  状态 */
#define RADAR_T_TARGET   0x0A04     /* LEN=20 目标 id/x/y/z */
#define RADAR_T_DISTANCE 0x0A16     /* LEN=8  flag + 距离(cm) */
#define RADAR_T_POS      0x0A17     /* LEN=8  位置 x/y */
#define RADAR_T_PHASE    0x0A13     /* LEN=12 相位 总/呼吸/心跳 */
#define RADAR_T_BREATH   0x0A14     /* LEN=4  呼吸速率 bpm */
#define RADAR_T_HEART    0x0A15     /* LEN=4  心跳速率 bpm */

/* ===================== 解析结果 ===================== */
typedef struct {
    float    breath_rate;       /* 呼吸速率 bpm（无数据=0） */
    float    heart_rate;        /* 心跳速率 bpm */
    float    distance_cm;       /* 目标距离 cm */
    float    pos_x, pos_y;      /* 目标位置 m */
    float    phase_total;       /* 总相位 */
    float    phase_breath;      /* 呼吸相位（±4，做波形/呼吸暂停检测用） */
    float    phase_heart;       /* 心跳相位（±2） */
    bool     present;           /* 是否有目标（来自 0x0A16 的 flag） */
    uint32_t frame_count;       /* 累计收到帧数（心跳） */
    int64_t  last_update_ms;    /* 最后一次更新时刻 */
} radar_state_t;

/* 初始化 UART1 并准备接收 */
esp_err_t radar_init(void);

/* 起后台任务：收字节 → 切帧 → 解析 → 更新状态（每 2 秒打一条日志） */
void radar_start_task(void);

/* 取最近一次解析结果；返回 false 表示还没收到过有效数据 */
bool radar_get_latest(radar_state_t *out);

#ifdef __cplusplus
}
#endif
