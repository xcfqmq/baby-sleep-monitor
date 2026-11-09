/*
 * fusion.c —— 多模态融合：用训练好的 MLP 做推理（边缘 AI）
 *
 * 不再用 if/else 规则，而是把传感器原始数据喂给训练好的神经网络：
 *   x = [呼吸率, 心率, 运动量, 亮度, 距离, 框宽, 框高, 遮挡]
 *   标准化 → h = ReLU(W1·x + b1) → logit = W2·h + b2 → argmax → 睡眠状态
 *
 * 权重和归一化参数来自 mlp_weights.h（由 train_mlp.py 生成）。
 * 类顺序已对齐 sleep_state_t：0=off_bed, 1=awake, 2=light_sleep, 3=deep_sleep。
 */
#include <limits.h>

#include "esp_timer.h"

#include "fusion.h"
#include "mlp_weights.h"

/* 派生的辅助字段仍用简单阈值判断（present/breath_valid） */
#define FUSION_BREATH_MIN    8     /* 呼吸率下限 bpm */
#define FUSION_BREATH_MAX    30    /* 呼吸率上限 bpm（成人） */
#define FUSION_DIST_OFF_BED  150   /* 距离 >= 此值(cm)视为离床 */
#define BREATH_ABNORMAL_SECS 10    /* 呼吸越界持续多少秒才判异常（去抖） */

/* 模型参数总量：W1 + B1 + W2 + B2 + mean + std（×4 字节 = 权重占用） */
#define MLP_TOTAL_PARAMS (MLP_IN_FEATURES * MLP_HIDDEN + MLP_HIDDEN + \
                          MLP_HIDDEN * MLP_CLASSES + MLP_CLASSES + 2 * MLP_IN_FEATURES)

static int s_breath_bad_secs = 0;  /* 呼吸越界已持续秒数 */

/* 推理耗时统计（微秒） */
static uint32_t s_infer_count = 0;
static int64_t  s_infer_total_us = 0;
static int64_t  s_infer_min_us = INT64_MAX;
static int64_t  s_infer_max_us = 0;

const char *fusion_state_str(sleep_state_t s)
{
    switch (s) {
    case SLEEP_OFF_BED:  return "off_bed";
    case SLEEP_AWAKE:    return "awake";
    case SLEEP_LIGHT:    return "light_sleep";
    case SLEEP_DEEP:     return "deep_sleep";
    case SLEEP_ABNORMAL: return "abnormal";
    default:             return "unknown";
    }
}

void fusion_update(const radar_state_t *r, const cam_state_t *c, fusion_result_t *out)
{
    /* 1. 原始字段 + 简单派生 */
    out->present     = (r->distance_cm > 0.0f && r->distance_cm < (float)FUSION_DIST_OFF_BED);
    out->breath_rate = r->breath_rate;
    out->heart_rate  = r->heart_rate;
    out->motion      = c->motion;
    out->brightness  = c->brightness;
    out->occluded    = c->occluded;
    out->box_x = c->x;
    out->box_y = c->y;
    out->box_w = c->w;
    out->box_h = c->h;
    out->breath_valid = (r->breath_rate >= FUSION_BREATH_MIN &&
                         r->breath_rate <= FUSION_BREATH_MAX);

    /* 呼吸异常检测（规则，独立于 MLP，不需要训练数据）：
     * 呼吸率持续越界（过慢 0~8 或过快 >30）N 秒才判异常；
     * breath_rate=0 是雷达失锁，不算异常（不会误报）。 */
    bool breath_bad = (r->breath_rate > 0.0f && r->breath_rate < FUSION_BREATH_MIN) ||
                      (r->breath_rate > FUSION_BREATH_MAX);
    if (breath_bad) {
        if (s_breath_bad_secs < BREATH_ABNORMAL_SECS) {
            s_breath_bad_secs++;
        }
    } else {
        s_breath_bad_secs = 0;
    }
    out->breath_abnormal = (s_breath_bad_secs >= BREATH_ABNORMAL_SECS);

    /* 2. 构建特征向量（顺序和 train_mlp.py 的 FEATURES 一致） */
    int64_t t0 = esp_timer_get_time();
    float x[MLP_IN_FEATURES];
    x[0] = r->breath_rate;
    x[1] = r->heart_rate;
    x[2] = (float)c->motion;
    x[3] = (float)c->brightness;
    x[4] = r->distance_cm;
    x[5] = (float)c->w;
    x[6] = (float)c->h;
    x[7] = c->occluded ? 1.0f : 0.0f;

    /* 3. 标准化（用训练时的 mean/std） */
    for (int i = 0; i < MLP_IN_FEATURES; i++) {
        x[i] = (x[i] - MLP_FEAT_MEAN[i]) / MLP_FEAT_STD[i];
    }

    /* 4. 隐藏层 h = ReLU(W1·x + b1) */
    float h[MLP_HIDDEN];
    for (int j = 0; j < MLP_HIDDEN; j++) {
        float s = MLP_B1[j];
        for (int i = 0; i < MLP_IN_FEATURES; i++) {
            s += x[i] * MLP_W1[i][j];
        }
        h[j] = (s > 0.0f) ? s : 0.0f;
    }

    /* 5. 输出层 + argmax（类顺序已和 sleep_state_t 对齐） */
    int   best_k = 0;
    float best_v = -1e30f;
    for (int k = 0; k < MLP_CLASSES; k++) {
        float s = MLP_B2[k];
        for (int j = 0; j < MLP_HIDDEN; j++) {
            s += h[j] * MLP_W2[j][k];
        }
        if (s > best_v) {
            best_v = s;
            best_k = k;
        }
    }
    /* 6. 记录推理耗时（微秒） */
    int64_t dt = esp_timer_get_time() - t0;
    s_infer_count++;
    s_infer_total_us += dt;
    if (dt < s_infer_min_us) s_infer_min_us = dt;
    if (dt > s_infer_max_us) s_infer_max_us = dt;

    out->state = (sleep_state_t)best_k;
}

void fusion_ai_stats(uint32_t *weight_bytes, uint32_t *infer_count,
                     int64_t *avg_us, int64_t *min_us, int64_t *max_us)
{
    if (weight_bytes) *weight_bytes = (uint32_t)(MLP_TOTAL_PARAMS * (int)sizeof(float));
    if (infer_count)  *infer_count = s_infer_count;
    if (avg_us)       *avg_us = s_infer_count ? (s_infer_total_us / (int64_t)s_infer_count) : 0;
    if (min_us)       *min_us = (s_infer_min_us == INT64_MAX) ? 0 : s_infer_min_us;
    if (max_us)       *max_us = s_infer_max_us;
}
