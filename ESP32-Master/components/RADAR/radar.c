/*
 * radar.c —— HLK-LD6002 呼吸心率雷达 UART 接收与协议解析实现
 *
 * 帧结构（已用实测数据验证）：
 *   [0]      SOF = 0x01
 *   [1..2]   帧ID（大端）
 *   [3..4]   LEN（大端）＝ DATA 长度
 *   [5..6]   TYPE（大端）
 *   [7]      头校验
 *   [8..]    DATA（LEN 字节，float32 小端）
 *   [8+LEN]  数据校验
 *
 * 注意：校验和算法文档给的是自定义 C 函数，这里不校验，靠 SOF + LEN 切帧
 *       （实测稳定）；另外模块每个值会同时发一遍 ASCII 文本（TYPE 0x0100），忽略即可。
 */
#include "radar.h"

#include <string.h>

#include "driver/uart.h"
#include "debug.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "RADAR";

#define RADAR_RX_BUF_SIZE  1024
#define RADAR_MAX_DATA     64

static SemaphoreHandle_t s_lock = NULL;
static radar_state_t     s_state;
static bool              s_have_data = false;

/* ---------------- 小端取值 ---------------- */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float get_f32(const uint8_t *p)
{
    float f;
    memcpy(&f, p, sizeof(f));      /* memcpy 避免非对齐访问 */
    return f;
}

/* ---------------- 初始化 ---------------- */
esp_err_t radar_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = RADAR_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(RADAR_UART_NUM, RADAR_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        D_LOGE(TAG, "uart_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(RADAR_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RADAR_UART_NUM, RADAR_PIN_TX, RADAR_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_lock = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));

    D_LOGI(TAG, "UART%d 就绪: RX=IO%d(接雷达TX0), TX=IO%d(接雷达RX0), %d bps",
             RADAR_UART_NUM, RADAR_PIN_RX, RADAR_PIN_TX, RADAR_BAUD);
    return ESP_OK;
}

/* ---------------- 解析一帧 ---------------- */
static void handle_frame(uint16_t type, const uint8_t *data, uint16_t len)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    s_state.frame_count++;
    s_state.last_update_ms = esp_timer_get_time() / 1000;

    switch (type) {
    case RADAR_T_BREATH:
        if (len >= 4) {
            s_state.breath_rate = get_f32(data);
        }
        break;
    case RADAR_T_HEART:
        if (len >= 4) {
            s_state.heart_rate = get_f32(data);
        }
        break;
    case RADAR_T_DISTANCE:
        if (len >= 8) {
            s_state.present     = (get_u32(data) != 0);   /* flag=1 表示有目标 */
            s_state.distance_cm = get_f32(data + 4);
        }
        break;
    case RADAR_T_POS:
        if (len >= 8) {
            s_state.pos_x = get_f32(data);
            s_state.pos_y = get_f32(data + 4);
        }
        break;
    case RADAR_T_PHASE:
        if (len >= 12) {
            s_state.phase_total  = get_f32(data);
            s_state.phase_breath = get_f32(data + 4);
            s_state.phase_heart  = get_f32(data + 8);
        }
        break;
    case RADAR_T_TARGET:
        if (len >= 16) {
            /* 目标信息帧里的位置和 0x0A17 重复，这里也存一份（兜底） */
            s_state.pos_x = get_f32(data + 4);
            s_state.pos_y = get_f32(data + 8);
        }
        break;
    default:
        break;   /* 状态帧(0x0A38/0x0A18/0x0F09)、文本帧(0x0100) 忽略 */
    }

    s_have_data = true;
    xSemaphoreGive(s_lock);
}

/* ---------------- 接收任务：按 SOF + LEN 切帧 ---------------- */
static void radar_task(void *arg)
{
    uint8_t  byte;
    uint8_t  hdr[7];                    /* 帧ID(2) LEN(2) TYPE(2) 头校验(1) */
    uint8_t  data[RADAR_MAX_DATA + 1];  /* DATA + 数据校验 */
    int64_t  last_log_ms = 0;
    uint32_t rx_bytes    = 0;

    while (1) {
        /* 周期性打印（独立于是否收到帧，方便排查接线/供电） */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_log_ms >= 2000) {
            last_log_ms = now_ms;
            radar_state_t st;
            if (radar_get_latest(&st)) {
                /* 视频演示时不刷屏：呼吸/心率/距离详情日志已关闭 */
                (void)st;
            } else if (rx_bytes == 0) {
                D_LOGI(TAG, "雷达无数据流入(0字节) → 查: 3.3V≥1A供电 / 共地 / 雷达TX0→IO%d / RX0→IO%d",
                         RADAR_PIN_RX, RADAR_PIN_TX);
            } else {
                D_LOGI(TAG, "收到%u字节但解析不出帧 → 查波特率(115200)或TX/RX是否接反", (unsigned)rx_bytes);
            }
        }

        /* 1) 找 SOF */
        int n = uart_read_bytes(RADAR_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        rx_bytes += (uint32_t)n;
        if (byte != 0x01) {
            continue;
        }

        /* 2) 读剩余 7 字节头 */
        if (uart_read_bytes(RADAR_UART_NUM, hdr, sizeof(hdr), pdMS_TO_TICKS(50)) != sizeof(hdr)) {
            continue;
        }
        uint16_t len = (uint16_t)((hdr[2] << 8) | hdr[3]);
        uint16_t typ = (uint16_t)((hdr[4] << 8) | hdr[5]);
        if (len == 0 || len > RADAR_MAX_DATA) {
            continue;                    /* 异常长度，丢弃重找 */
        }

        /* 3) 读 DATA + 数据校验 */
        if (uart_read_bytes(RADAR_UART_NUM, data, len + 1, pdMS_TO_TICKS(50)) != len + 1) {
            continue;
        }
        rx_bytes += (uint32_t)(len + 1);

        /* 4) 分发 */
        handle_frame(typ, data, len);
    }
}

void radar_start_task(void)
{
    xTaskCreate(radar_task, "radar", 4 * 1024, NULL, 4, NULL);
}

/* ---------------- 取最新状态 ---------------- */
bool radar_get_latest(radar_state_t *out)
{
    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_have_data) {
            *out = s_state;
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}
