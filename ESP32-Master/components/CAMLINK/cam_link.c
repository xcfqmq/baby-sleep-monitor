/*
 * cam_link.c —— 主机侧 I2C 主机实现（读取摄像头从机的寄存器表）
 */
#include "cam_link.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "CAMLINK";

static i2c_master_bus_handle_t    s_bus = NULL;
static i2c_master_dev_handle_t    s_dev = NULL;
static SemaphoreHandle_t          s_lock = NULL;
static cam_state_t                s_latest;
static bool                       s_have_latest = false;

esp_err_t cam_link_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;                      /* 已经初始化过 */
    }

    /* 第二条 I2C 总线：端口交给驱动自动选（BMI270 占着另一条） */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,            /* 自动选择空闲端口 */
        .sda_io_num        = CAM_LINK_SDA_GPIO,
        .scl_io_num        = CAM_LINK_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 打开内部上拉 */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        D_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CAM_LINK_ADDR,
        .scl_speed_hz    = CAM_LINK_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        D_LOGE(TAG, "i2c_master_bus_add_device 失败: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    D_LOGI(TAG, "I2C 主机就绪: SDA=IO%d, SCL=IO%d, 从机地址 0x%02X, %d Hz",
             CAM_LINK_SDA_GPIO, CAM_LINK_SCL_GPIO, CAM_LINK_ADDR, CAM_LINK_FREQ_HZ);
    return ESP_OK;
}

esp_err_t cam_link_read(cam_state_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = 0x00;                     /* 从偏移 0 开始，一次读回整张表 */
    uint8_t buf[CAM_LINK_REG_SIZE];

    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 200);
    if (err != ESP_OK) {
        return err;
    }

    cam_state_t st;
    st.magic       = buf[0];
    st.fw_version  = buf[1];
    st.status      = buf[2];
    st.occluded    = (buf[2] & 0x02) ? true : false;
    st.x           = (int16_t)((uint16_t)buf[4]  | ((uint16_t)buf[5]  << 8));
    st.y           = (int16_t)((uint16_t)buf[6]  | ((uint16_t)buf[7]  << 8));
    st.w           = (int16_t)((uint16_t)buf[8]  | ((uint16_t)buf[9]  << 8));
    st.h           = (int16_t)((uint16_t)buf[10] | ((uint16_t)buf[11] << 8));
    st.brightness  = (uint16_t)((uint16_t)buf[12] | ((uint16_t)buf[13] << 8));
    st.motion      = (uint16_t)((uint16_t)buf[14] | ((uint16_t)buf[15] << 8));
    st.frame_count = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
                     ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);

    /* 完整性校验：魔术字 + 校验和（字节 3，自身除外）。
     * 注意：不要用「连读两次比对」来防撕裂读——S3 的 I2C 从机 RAM 模式
     * 扛不住背靠背的连续读（第二次会读出垃圾，导致每次都判失败）。
     * 这里改成「校验和过滤」：读坏了就丢弃这一拍，等下一拍。 */
    uint8_t cs = 0;
    for (int i = 0; i < CAM_LINK_REG_SIZE; i++) {
        if (i != CAM_LINK_REG_CHECKSUM) {
            cs = (uint8_t)(cs + buf[i]);
        }
    }
    if (st.magic != CAM_LINK_MAGIC || cs != buf[CAM_LINK_REG_CHECKSUM]) {
        return ESP_ERR_INVALID_RESPONSE;    /* 撕裂读，或从机不是我们的固件 */
    }

    *out = st;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_latest = st;
        s_have_latest = true;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

bool cam_link_get_latest(cam_state_t *out)
{
    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_have_latest) {
            *out = s_latest;
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

/* ---------------- 后台轮询任务 ---------------- */
static void cam_link_task(void *arg)
{
    const uint32_t interval_ms = (uint32_t)(uintptr_t)arg;
    cam_state_t    st;
    uint32_t       fail_count = 0;

    while (1) {
        esp_err_t err = cam_link_read(&st);
        if (err == ESP_OK) {
            if (fail_count > 0) {
                D_LOGI(TAG, "从机恢复在线（之前连续失败 %u 次）", (unsigned)fail_count);
                fail_count = 0;
            }
            /* 视频演示时不刷屏：从机详情日志已关闭 */
            (void)st;
        } else {
            fail_count++;
            /* 每 10 次才打一条，避免刷屏 */
            if (fail_count % 10 == 1) {
                D_LOGW(TAG, "读从机失败: %s（检查 IO%d/IO%d 接线与共地，累计失败 %u 次）",
                         esp_err_to_name(err), CAM_LINK_SDA_GPIO, CAM_LINK_SCL_GPIO,
                         (unsigned)fail_count);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

void cam_link_start_task(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        interval_ms = 500;
    }
    xTaskCreate(cam_link_task, "cam_link", 4 * 1024, (void *)(uintptr_t)interval_ms, 4, NULL);
}
