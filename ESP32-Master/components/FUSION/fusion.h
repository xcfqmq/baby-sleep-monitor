/*
 * fusion.h —— 多模态融合：把雷达 + 摄像头特征合成"睡眠状态"
 *
 * 输入：
 *   - 雷达（radar_state_t）：呼吸率、心率、有无目标、距离
 *   - 摄像头（cam_state_t）：运动量、亮度
 * 输出：
 *   - 睡眠状态（5 种）：离床 / 清醒 / 浅睡 / 深睡 / 异常
 *
 * v1 是规则版（阈值可调），v2 会换成训练出来的 MLP（边缘 AI）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cam_link.h"
#include "radar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEP_OFF_BED  = 0,   /* 离床：雷达没检测到目标 */
    SLEEP_AWAKE    = 1,   /* 清醒/活动：运动量大 */
    SLEEP_LIGHT    = 2,   /* 浅睡：轻微运动 */
    SLEEP_DEEP     = 3,   /* 深睡：静止 + 呼吸正常 */
    SLEEP_ABNORMAL = 4,   /* 异常：呼吸率越界（过快/过慢） */
} sleep_state_t;

typedef struct {
    sleep_state_t state;
    bool    present;         /* 是否在床（雷达有无目标） */
    bool    breath_valid;    /* 呼吸率是否在正常范围 */
    bool    breath_abnormal; /* 呼吸异常（过慢/过快持续 N 秒，规则检测） */
    float   breath_rate;     /* bpm */
    float   heart_rate;      /* bpm */
    uint16_t motion;         /* 0~4095（摄像头运动量） */
    uint16_t brightness;     /* 0~4095（摄像头亮度） */
    bool     occluded;       /* 画面被遮挡（摄像头） */
    int16_t  box_x, box_y;   /* 运动区域包围盒（图像坐标） */
    int16_t  box_w, box_h;
} fusion_result_t;

/* 状态转字符串（用于 JSON 和日志） */
const char *fusion_state_str(sleep_state_t s);

/* 根据雷达 + 摄像头数据算一次融合结论 */
void fusion_update(const radar_state_t *r, const cam_state_t *c, fusion_result_t *out);

/* 取边缘 AI（MLP）统计：权重字节数 + 推理耗时（微秒） */
void fusion_ai_stats(uint32_t *weight_bytes, uint32_t *infer_count,
                     int64_t *avg_us, int64_t *min_us, int64_t *max_us);

#ifdef __cplusplus
}
#endif
