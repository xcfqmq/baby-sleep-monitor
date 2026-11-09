/*
 * features.c —— 图像特征提取实现（亮度 + 运动量）
 *
 * 算法（和原来 main.c 里的一字不差，只是搬了家）：
 *   - 把 RGB565 帧降采样成 32x32 灰度网格；
 *   - 亮度 = 全画面平均灰度，归一化到 0~4095；
 *   - 运动量 = 本帧网格与上一帧的平均绝对差，归一化到 0~4095。
 */
#include "features.h"

#include <string.h>

static uint8_t s_grid[FEAT_GRID_N];   /* 当前帧灰度网格 */
static uint8_t s_prev[FEAT_GRID_N];   /* 上一帧灰度网格 */
static bool    s_have_prev = false;
static uint32_t s_dark_frames = 0;    /* 亮度持续过低的帧数（遮挡检测） */

/* 遮挡：亮度(0~4095)持续低于此阈值多少帧才判定遮挡 */
#define OCCLUSION_BRIGHT_TH  150
#define OCCLUSION_FRAMES     30        /* 30 帧 ≈ 3 秒 @10fps */

/* 运动区域：单格灰度差(0~63)超过此值才计入包围盒 */
#define MOTION_CELL_TH       15
#define MOTION_CELL_MIN      5        /* 至少这么多格动，才算有效运动（滤噪声） */

/*
 * 把 RGB565 帧降采样成 32x32 灰度网格，并返回全画面平均亮度（0~4095）。
 * 灰度用 RGB565 现场算：luma = (2R + 4G + B) / 8，范围 0~63。
 * 为了省时间，每块内隔行/隔列采样。
 */
static uint32_t frame_to_grid(const camera_fb_t *fb, uint8_t *grid)
{
    const int      W   = (int)fb->width;
    const int      H   = (int)fb->height;
    const uint8_t *buf = fb->buf;
    const int      sx  = W / FEAT_GRID_W;        /* 每格宽度 */
    const int      sy  = H / FEAT_GRID_H;        /* 每格高度 */
    uint32_t       luma_sum = 0;

    for (int gy = 0; gy < FEAT_GRID_H; gy++) {
        for (int gx = 0; gx < FEAT_GRID_W; gx++) {
            uint32_t acc = 0;
            int      cnt = 0;

            for (int y = gy * sy; y < (gy + 1) * sy; y += 2) {
                const uint8_t *row = buf + (size_t)y * W * 2;
                for (int x = gx * sx; x < (gx + 1) * sx; x += 2) {
                    uint16_t px = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                    int r = (px >> 11) & 0x1F;                  /* 5 位 */
                    int g = (px >> 5) & 0x3F;                   /* 6 位 */
                    int b = px & 0x1F;                          /* 5 位 */
                    acc += (uint32_t)((r * 2 + g * 4 + b) >> 3); /* 0~63 */
                    cnt++;
                }
            }
            
            uint8_t v = cnt ? (uint8_t)(acc / (uint32_t)cnt) : 0;
            grid[gy * FEAT_GRID_W + gx] = v;
            luma_sum += v;
        }
    }

    /* 归一到 0~4095，方便主机侧统一量纲 */
    return (luma_sum * 4095U) / (FEAT_GRID_N * 63U);
}

/* 两帧网格的平均绝对差（0~63），衡量“动了多少” */
static uint32_t grid_motion(const uint8_t *a, const uint8_t *b)
{
    uint32_t sum = 0;
    for (int i = 0; i < FEAT_GRID_N; i++) {
        int d = (int)a[i] - (int)b[i];
        sum += (uint32_t)(d < 0 ? -d : d);
    }
    return sum / FEAT_GRID_N;
}

void features_update(const camera_fb_t *fb, vision_features_t *out)
{
    uint32_t brightness = frame_to_grid(fb, s_grid);

    /* 运动量 + 运动区域包围盒（和上一帧对比） */
    uint32_t motion = 0;
    int16_t  bx = 0, by = 0, bw = 0, bh = 0;
    if (s_have_prev) {
        motion = (grid_motion(s_prev, s_grid) * 4095U) / 63U;

        /* 找"哪些格子动了"，求最小包围矩形 */
        int min_gx = FEAT_GRID_W, min_gy = FEAT_GRID_H;
        int max_gx = -1, max_gy = -1;
        int moved_cells = 0;
        for (int gy = 0; gy < FEAT_GRID_H; gy++) {
            for (int gx = 0; gx < FEAT_GRID_W; gx++) {
                int a = s_grid[gy * FEAT_GRID_W + gx];
                int b = s_prev[gy * FEAT_GRID_W + gx];
                int d = a - b;
                if (d < 0) d = -d;
                if (d > MOTION_CELL_TH) {
                    moved_cells++;
                    if (gx < min_gx) min_gx = gx;
                    if (gx > max_gx) max_gx = gx;
                    if (gy < min_gy) min_gy = gy;
                    if (gy > max_gy) max_gy = gy;
                }
            }
        }
        /* 至少 MOTION_CELL_MIN 个格子动才算有效运动，滤掉零星噪声 */
        if (moved_cells >= MOTION_CELL_MIN && max_gx >= min_gx) {
            /* 映射回图像坐标（0~width / 0~height） */
            bx = (int16_t)(min_gx * (int)fb->width  / FEAT_GRID_W);
            by = (int16_t)(min_gy * (int)fb->height / FEAT_GRID_H);
            bw = (int16_t)((max_gx - min_gx + 1) * (int)fb->width  / FEAT_GRID_W);
            bh = (int16_t)((max_gy - min_gy + 1) * (int)fb->height / FEAT_GRID_H);
        }
    }
    memcpy(s_prev, s_grid, sizeof(s_grid));
    s_have_prev = true;

    /* 遮挡检测：亮度持续过低 */
    if (brightness < OCCLUSION_BRIGHT_TH) {
        if (s_dark_frames < 0xFFFFFFFF) {
            s_dark_frames++;
        }
    } else {
        s_dark_frames = 0;
    }

    out->brightness = brightness;
    out->motion     = motion;
    out->occluded   = (s_dark_frames >= OCCLUSION_FRAMES);
    out->box_x = bx;
    out->box_y = by;
    out->box_w = bw;
    out->box_h = bh;
}

void features_reset(void)
{
    s_have_prev = false;
}
