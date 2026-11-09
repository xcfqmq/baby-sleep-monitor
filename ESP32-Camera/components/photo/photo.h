/*
 * photo.h —— 摄像头组件（ESP32-S3-Cam，GC2145）
 *
 * 职责：把「摄像头初始化 / 取帧 / 归还帧 / 打印信息」封装起来，
 *       上层（main.c 或后续的 AI 任务）只调这几个函数，不关心底层引脚和驱动细节。
 *
 * 板子引脚来源：资料\02 人脸识别\02 ESP32S3-Cam人脸识别程序\FaceDetection\camera_setting.h
 *              （CAMERA_MODEL_ESP32S3_EYE 段）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 配置 ===================== */
typedef struct {
    framesize_t frame_size;    /* 分辨率：AI 推理用 FRAMESIZE_240X240；要看图用 FRAMESIZE_QVGA/VGA */
    pixformat_t pixel_format;  /* 像素格式：AI 用 PIXFORMAT_RGB565；传图用 PIXFORMAT_JPEG */
    uint8_t     fb_count;      /* 帧缓冲个数：1 = 抓一帧停一次；2+ = 连续采集（推流/AI 用 2） */
    int         xclk_freq_hz;  /* 摄像头主时钟：本板厂商实测 15MHz */
    int         jpeg_quality;  /* JPEG 质量 0~63，数字越小越清晰（仅 PIXFORMAT_JPEG 有效） */
} photo_config_t;

/* 默认配置：240x240 / RGB565 / 2 缓冲 / 15MHz —— 对应端侧 AI 推理的数据格式 */
photo_config_t photo_default_config(void);

/* ===================== 生命周期 ===================== */
/*
 * 初始化摄像头。cfg 传 NULL 表示用默认配置（photo_default_config）。
 * 需要 sdkconfig 里开启 PSRAM（帧缓冲放在 PSRAM），否则会失败。
 */
esp_err_t photo_init(const photo_config_t *cfg);

/* 摄像头是否已就绪（初始化成功过） */
bool photo_is_ready(void);

/* ===================== 取帧 ===================== */
/*
 * 取一帧。返回的 camera_fb_t 用完必须调 photo_return_frame 归还，
 * 否则帧缓冲会被耗尽、后续取帧一直失败。
 * 返回 NULL 表示取帧失败（可用 esp_err_to_name 之类查日志）。
 */
camera_fb_t *photo_get_frame(void);
void         photo_return_frame(camera_fb_t *fb);

/* ===================== 信息 ===================== */
/* 打印传感器型号（GC2145/OV2640…）、PID 和当前配置，初始化后自动调用一次 */
void photo_print_info(void);

#ifdef __cplusplus
}
#endif
