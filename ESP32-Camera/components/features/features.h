/*
 * features.h —— 图像特征提取（纯算法，与业务解耦）
 *
 * 职责：把一帧图像变成几个"特征数字"（亮度、运动量……），供上层使用。
 * 目前只有亮度 + 运动量；以后接 AI 后在这里加：遮挡标志、运动区域包围盒、人脸框等。
 *
 * 注意：内部会保存"上一帧"用于算运动量，所以 features_update() 必须按时间顺序
 *       逐帧调用；从机重启/断流时调 features_reset() 清掉上一帧，避免把断档当成
 *       巨大运动量。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FEAT_GRID_W  32                          /* 降采样网格宽 */
#define FEAT_GRID_H  32                          /* 降采样网格高 */
#define FEAT_GRID_N  (FEAT_GRID_W * FEAT_GRID_H)

typedef struct {
    uint32_t brightness;     /* 0~4095 画面平均亮度 */
    uint32_t motion;         /* 0~4095 与上一帧的运动量 */
    bool     occluded;       /* 遮挡标志：画面持续过暗（被子盖住摄像头/脸） */
    int16_t  box_x, box_y;   /* 运动区域包围盒左上角（图像坐标，无运动为 0） */
    int16_t  box_w, box_h;   /* 运动区域包围盒宽高 */
} vision_features_t;

/* 处理一帧，把算出的特征写进 out。必须按时间顺序逐帧调用。 */
void features_update(const camera_fb_t *fb, vision_features_t *out);

/* 清掉内部的"上一帧"（从机重启/断流后调用） */
void features_reset(void);

#ifdef __cplusplus
}
#endif
