/*
 * main.c —— ESP32-S3-Cam 从机固件
 *
 * 流程（只看这里就够）：初始化摄像头 + I2C 从机 → 主循环
 *   （抓帧 → 提特征 → 写寄存器表 → 延时 100ms）
 *
 * 具体实现分散在三个组件里，各有各的职责：
 *   - photo    组件：摄像头（初始化/取帧/归还）
 *   - features 组件：特征提取（亮度/运动量）
 *   - report   组件：I2C 从机上报（0x52 寄存器表）
 *
 * 后续步骤（TODO）：接入端侧 AI（ESP-DL），把检测框填进 report_set_detect()
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "features.h"
#include "photo.h"
#include "report.h"

static const char *TAG = "APP";

#define FRAME_INTERVAL_MS 100   /* 10fps：运动检测需要连续帧 */
#define LOG_EVERY_N       10    /* 每 10 帧打一条日志（1 秒） */

static uint32_t s_frames = 0;

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32-S3-Cam 从机启动（摄像头 + 特征 + I2C 上报）=====");

    /* 1) 摄像头 */
    esp_err_t err = photo_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: %s", esp_err_to_name(err));
        return;
    }

    /* 2) I2C 从机上报 */
    err = report_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 从机初始化失败: %s（检查 IO%d/IO%d 接线）",
                 esp_err_to_name(err), REPORT_I2C_SDA_GPIO, REPORT_I2C_SCL_GPIO);
        return;
    }

    ESP_LOGI(TAG, "开始运行：10fps 抓帧 → 特征 → 更新 I2C 寄存器表");

    while (1) {
        int64_t      t0 = esp_timer_get_time();
        camera_fb_t *fb = photo_get_frame();
        if (fb == NULL) {
            ESP_LOGW(TAG, "抓帧失败");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t grab_us = esp_timer_get_time() - t0;

        /* ---- 特征提取 ---- */
        vision_features_t feat;
        features_update(fb, &feat);

        s_frames++;

        /* ---- 写进 I2C 寄存器表（主机随时可读） ---- */
        report_set_status(REPORT_STATUS_RUN);
        report_set_occluded(feat.occluded);
        report_set_detect(feat.box_x, feat.box_y, feat.box_w, feat.box_h);
        report_set_env((uint16_t)feat.brightness, (uint16_t)feat.motion);
        report_set_frame_count(s_frames);
        esp_err_t werr = report_flush();

        if (s_frames % LOG_EVERY_N == 0) {
            ESP_LOGI(TAG, "帧 %u | 亮度 %u | 运动 %u | 遮挡 %d | 框(%d,%d,%d,%d)%s",
                     (unsigned)s_frames,
                     (unsigned)feat.brightness, (unsigned)feat.motion,
                     (int)feat.occluded,
                     (int)feat.box_x, (int)feat.box_y, (int)feat.box_w, (int)feat.box_h,
                     (werr == ESP_OK) ? "" : "  [寄存器写入失败!]");
        }

        photo_return_frame(fb);
        vTaskDelay(pdMS_TO_TICKS(FRAME_INTERVAL_MS));
    }
}
