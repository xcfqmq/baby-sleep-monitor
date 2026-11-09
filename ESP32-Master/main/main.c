#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "debug.h"
#include "wifi.h"
#include "sensor.h"
#include "mqtt.h"
#include "cam_link.h"
#include "radar.h"
#include "fusion.h"
#include "oled.h"
#include "cJSON.h"

static const char *TAG = "MAIN";

/* ======================= 采样/发布配置 ======================= */
#define SAMPLE_PERIOD_MS   10     /* 采样间隔 10ms -> 约 100 Hz */
#define SAMPLES_PER_MSG    50     /* 每包塞 50 个采样点 -> 每 0.5 秒发一包 */
#define MQTT_TOPIC         "esp32/vibration"
#define MQTT_TOPIC_RADAR   "esp32/radar"
#define MQTT_TOPIC_CAMERA  "esp32/camera"
#define MQTT_TOPIC_FUSION  "esp32/fusion"

static uint32_t s_msg_count = 0;
static fusion_result_t s_fusion;
static bool s_have_fusion = false;
static radar_state_t s_radar;
static bool s_have_radar = false;

/* 等 MQTT 连上（最多 timeout_ms），连上返回 true */
static bool wait_mqtt(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!MQTT_is_connected() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return MQTT_is_connected();
}

void app_main(void)
{
    /* NVS：WiFi 驱动需要用到 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 1. WiFi 连上并拿到 IP（自动重连由 wifi 组件负责） */
    WIFI_INIT();
    if (!WIFI_wait_ip(15000)) {
        D_LOGW(TAG, "等待 IP 超时，WiFi 后台会继续自动重连");
    }

    /* 2.  MQTT */
    MQTT_init();
    if (!wait_mqtt(10000)) {
        D_LOGW(TAG, "等待 MQTT 连接超时，请检查 mqtt.c 顶部的 MQTT_BROKER_URI 是否填对");
    }

    /* 2.5 摄像头从机链路：I2C 读 ESP32-S3-Cam（地址 0x52，SDA=IO17 / SCL=IO18） */
    if (cam_link_init() == ESP_OK) {
        cam_link_start_task(500);            /* 每 500ms 读一次并打印 */
    } else {
        D_LOGW(TAG, "摄像头从机链路初始化失败（检查 IO17/IO18 接线）");
    }

    /* 2.6 毫米波雷达：LD6002 呼吸心率（UART1，RX=IO9 / TX=IO10） */
    if (radar_init() == ESP_OK) {
        radar_start_task();                  /* 后台解析呼吸/心率/距离/相位并打印 */
    } else {
        D_LOGW(TAG, "雷达初始化失败（检查 IO9/IO10 接线）");
    }

    /* 2.7 OLED 显示 */
    if (oled_init() != ESP_OK) {
        D_LOGW(TAG, "OLED 初始化失败（检查 IO7/IO8 接线）");
    }

    TickType_t   last_report = xTaskGetTickCount();
    radar_state_t ra;
    cam_state_t   cs;
    /* 3. 主循环：采样 -> 攒够一包 -> 打包 JSON -> 发布 */
    while (1)
    {
        /* ---- A. 读雷达 + 发布（有数据才发）---- */
        bool have_radar = radar_get_latest(&ra);
        if (have_radar)
        {
            s_radar = ra;
            s_have_radar = true;

            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "breath_rate", ra.breath_rate);
            cJSON_AddNumberToObject(root, "heart_rate",  ra.heart_rate);
            cJSON_AddNumberToObject(root, "distance_cm", ra.distance_cm);
            cJSON_AddBoolToObject  (root, "present",     ra.present);

            char *json = cJSON_PrintUnformatted(root);
            if (json && MQTT_is_connected()) {
                MQTT_publish(MQTT_TOPIC_RADAR, json);
                s_msg_count++;
            }
            if (json) cJSON_free(json);
            cJSON_Delete(root);
        }

        /* ---- A2. 读摄像头从机 + 发布（有数据才发） ---- */
        bool have_camera = cam_link_get_latest(&cs);
        if (have_camera) {
                cJSON *root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "status",      cs.status);
                cJSON_AddBoolToObject  (root, "occluded",    cs.occluded);
                cJSON_AddNumberToObject(root, "brightness",  cs.brightness);
                cJSON_AddNumberToObject(root, "motion",      cs.motion);
                cJSON_AddNumberToObject(root, "box_x",       cs.x);
                cJSON_AddNumberToObject(root, "box_y",       cs.y);
                cJSON_AddNumberToObject(root, "box_w",       cs.w);
                cJSON_AddNumberToObject(root, "box_h",       cs.h);
                cJSON_AddNumberToObject(root, "frame_count", cs.frame_count);

                char *json = cJSON_PrintUnformatted(root);
                if (json && MQTT_is_connected()) {
                    MQTT_publish(MQTT_TOPIC_CAMERA, json);
                    s_msg_count++;
                }
                if (json) cJSON_free(json);
                cJSON_Delete(root);
        }

        /* ---- A3. 融合（雷达 + 摄像头）→ esp32/fusion ---- */
        if (have_radar && have_camera) {
            fusion_result_t fr;
            fusion_update(&ra, &cs, &fr);
            s_fusion = fr;
            s_have_fusion = true;

            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "state",        fusion_state_str(fr.state));
            cJSON_AddBoolToObject  (root, "present",      fr.present);
            cJSON_AddBoolToObject  (root, "breath_valid", fr.breath_valid);
            cJSON_AddBoolToObject  (root, "breath_abnormal", fr.breath_abnormal);
            cJSON_AddNumberToObject(root, "breath_rate",  fr.breath_rate);
            cJSON_AddNumberToObject(root, "heart_rate",   fr.heart_rate);
            cJSON_AddNumberToObject(root, "motion",       fr.motion);
            cJSON_AddNumberToObject(root, "brightness",   fr.brightness);
            cJSON_AddBoolToObject  (root, "occluded",     fr.occluded);
            cJSON_AddNumberToObject(root, "box_x",        fr.box_x);
            cJSON_AddNumberToObject(root, "box_y",        fr.box_y);
            cJSON_AddNumberToObject(root, "box_w",        fr.box_w);
            cJSON_AddNumberToObject(root, "box_h",        fr.box_h);

            char *json = cJSON_PrintUnformatted(root);
            if (json && MQTT_is_connected()) {
                MQTT_publish(MQTT_TOPIC_FUSION, json);
                s_msg_count++;
            }
            if (json) cJSON_free(json);
            cJSON_Delete(root);

            D_LOGI(TAG, "融合: %s | 呼吸 %.1f 心率 %.1f 运动 %u%s",
                     fusion_state_str(fr.state), fr.breath_rate, fr.heart_rate, fr.motion,
                     fr.breath_abnormal ? "  [呼吸异常!]" : "");
        }

        /* ---- B. 每秒打印状态 ---- */
        TickType_t now = xTaskGetTickCount();
        if (now - last_report >= pdMS_TO_TICKS(1000)) {
            last_report = now;
            /* 边缘 AI 统计：权重大小 + 推理耗时 */
            uint32_t ai_bytes, ai_count;
            int64_t  ai_avg, ai_min, ai_max;
            fusion_ai_stats(&ai_bytes, &ai_count, &ai_avg, &ai_min, &ai_max);
            D_LOGI(TAG, "AI: 权重%uB 推理%u次 平均%lldus 最快%lldus 最慢%lldus",
                     (unsigned)ai_bytes, (unsigned)ai_count,
                     (long long)ai_avg, (long long)ai_min, (long long)ai_max);

            /* 更新 OLED 显示（呼吸异常时 ST 行显示 ABNORMAL） */
            oled_show_status(WIFI_is_connected(), MQTT_is_connected(),
                             WIFI_get_ip_str(),
                             (s_have_fusion && s_fusion.breath_abnormal) ? "ABNORMAL"
                                 : (s_have_fusion ? fusion_state_str(s_fusion.state) : "--"),
                             s_have_radar ? s_radar.breath_rate : 0.0f,
                             s_have_radar ? s_radar.heart_rate : 0.0f,
                             s_have_radar ? s_radar.distance_cm : 0.0f);
        }

        /* ---- C. 每秒循环一次 ---- */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}   /* app_main 结束 */
