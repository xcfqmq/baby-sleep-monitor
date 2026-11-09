/*
 * photo.c —— 摄像头组件实现（ESP32-S3-Cam，GC2145）
 *
 * 说明：
 *   - 图像数据走 DVP 并口（D0~D7 + PCLK/VSYNC/HREF/XCLK），由 esp32-camera 驱动 + DMA 完成；
 *     引脚表里的 SIOD/SIOC 是 SCCB（类 I2C），只用于读写摄像头寄存器（分辨率/曝光等），
 *     速率只有 100~400kHz，但和图像数据无关。
 *   - 帧缓冲放在 PSRAM（CAMERA_FB_IN_PSRAM），所以 sdkconfig 必须开 PSRAM。
 */
#include "photo.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PHOTO";

/* ===================== 引脚定义（幻尔 ESP32-S3-Cam = ESP32-S3-EYE 引脚） ===================== */
#define PHOTO_PIN_PWDN   (-1)   /* 本板未引出 PWDN */
#define PHOTO_PIN_RESET  (-1)   /* 用软件复位 */
#define PHOTO_PIN_XCLK   15
#define PHOTO_PIN_SIOD   4      /* SCCB 数据（配置用，不是图像通路） */
#define PHOTO_PIN_SIOC   5      /* SCCB 时钟 */
#define PHOTO_PIN_D7     16
#define PHOTO_PIN_D6     17
#define PHOTO_PIN_D5     18
#define PHOTO_PIN_D4     12
#define PHOTO_PIN_D3     10
#define PHOTO_PIN_D2     8
#define PHOTO_PIN_D1     9
#define PHOTO_PIN_D0     11
#define PHOTO_PIN_VSYNC  6
#define PHOTO_PIN_HREF   7
#define PHOTO_PIN_PCLK   13

#define PHOTO_XCLK_FREQ_HZ   (15000000)   /* 厂商值 15MHz */

static bool        s_ready        = false;
static framesize_t s_frame_size   = FRAMESIZE_240X240;
static pixformat_t s_pixel_format = PIXFORMAT_RGB565;

photo_config_t photo_default_config(void)
{
    photo_config_t cfg = {
        .frame_size   = FRAMESIZE_240X240,  /* 端侧 AI 推理用 240x240 */
        .pixel_format = PIXFORMAT_RGB565,   /* AI 用 RGB565；要传图就改成 PIXFORMAT_JPEG */
        .fb_count     = 2,
        .xclk_freq_hz = PHOTO_XCLK_FREQ_HZ,
        .jpeg_quality = 16,
    };
    return cfg;
}

esp_err_t photo_init(const photo_config_t *cfg)
{
    photo_config_t c = cfg ? *cfg : photo_default_config();

    camera_config_t config = {
        .pin_pwdn     = PHOTO_PIN_PWDN,
        .pin_reset    = PHOTO_PIN_RESET,
        .pin_xclk     = PHOTO_PIN_XCLK,
        .pin_sccb_sda = PHOTO_PIN_SIOD,
        .pin_sccb_scl = PHOTO_PIN_SIOC,
        .pin_d7       = PHOTO_PIN_D7,
        .pin_d6       = PHOTO_PIN_D6,
        .pin_d5       = PHOTO_PIN_D5,
        .pin_d4       = PHOTO_PIN_D4,
        .pin_d3       = PHOTO_PIN_D3,
        .pin_d2       = PHOTO_PIN_D2,
        .pin_d1       = PHOTO_PIN_D1,
        .pin_d0       = PHOTO_PIN_D0,
        .pin_vsync    = PHOTO_PIN_VSYNC,
        .pin_href     = PHOTO_PIN_HREF,
        .pin_pclk     = PHOTO_PIN_PCLK,

        .xclk_freq_hz = c.xclk_freq_hz,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = c.pixel_format,
        .frame_size   = c.frame_size,
        .jpeg_quality = c.jpeg_quality,
        .fb_count     = c.fb_count,
        .fb_location  = CAMERA_FB_IN_PSRAM,     /* 帧缓冲放 PSRAM（需开 CONFIG_SPIRAM） */
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY, /* 和厂商例程一致 */
    };

    ESP_LOGI(TAG, "正在初始化摄像头（XCLK %d Hz）...", c.xclk_freq_hz);

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init 失败: %s (0x%x)", esp_err_to_name(err), err);
        s_ready = false;
        return err;
    }

    s_frame_size   = c.frame_size;
    s_pixel_format = c.pixel_format;
    s_ready        = true;

    photo_print_info();
    return ESP_OK;
}

bool photo_is_ready(void)
{
    return s_ready;
}

camera_fb_t *photo_get_frame(void)
{
    if (!s_ready) {
        return NULL;
    }
    return esp_camera_fb_get();
}

void photo_return_frame(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

void photo_print_info(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGW(TAG, "无法获取 sensor 句柄");
        return;
    }

    const char *name = "未知型号";
    switch (s->id.PID) {
        case GC2145_PID:   name = "GC2145 (本板板载)"; break;
        case OV2640_PID:   name = "OV2640";           break;
        case OV3660_PID:   name = "OV3660";           break;
        case OV5640_PID:   name = "OV5640";           break;
        case OV7670_PID:   name = "OV7670";           break;
        case OV7725_PID:   name = "OV7725";           break;
        case GC0308_PID:   name = "GC0308";           break;
        case GC032A_PID:   name = "GC032A";           break;
        case SC030IOT_PID: name = "SC030IOT";         break;
        case SC031GS_PID:  name = "SC031GS";          break;
        case NT99141_PID:  name = "NT99141";          break;
        case HM1055_PID:   name = "HM1055";           break;
        case HM0360_PID:   name = "HM0360";           break;
        default: break;
    }

    ESP_LOGI(TAG, "传感器: %s", name);
    ESP_LOGI(TAG, "  PID=0x%04X VER=0x%02X MIDL=0x%02X MIDH=0x%02X",
             (unsigned)s->id.PID, (unsigned)s->id.VER,
             (unsigned)s->id.MIDL, (unsigned)s->id.MIDH);
    ESP_LOGI(TAG, "配置: 分辨率=%d, 像素格式=%d, 帧缓冲在 PSRAM",
             (int)s_frame_size, (int)s_pixel_format);
}
