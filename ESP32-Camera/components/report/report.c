/*
 * report.c —— 从机上报实现（I2C 从机 + RAM 寄存器表）
 *
 * 关键点：
 *   1) 端口选择：摄像头的 SCCB 已经占了 I2C 端口 1（启动日志 sccb_i2c_port=1），
 *      所以从机显式用端口 0，避免打架。
 *   2) access_ram_en = true 打开 RAM 寄存器模式，之后用 i2c_slave_write_ram()
 *      更新寄存器表，硬件负责应答主机的读写。
 *   3) 引脚：SDA=IO47 / SCL=IO48（板子右侧扩展接口、厂商例程同款）。
 */
#include "report.h"

#include <string.h>

#include "driver/i2c_slave.h"
#include "esp_log.h"

static const char *TAG = "REPORT";

static i2c_slave_dev_handle_t s_slave = NULL;
static uint8_t                s_regs[REPORT_REG_SIZE];

/* ---------- 小工具：往寄存器表里写小端数据 ---------- */
static void put_u16(int off, uint16_t v)
{
    s_regs[off]     = (uint8_t)(v & 0xFF);
    s_regs[off + 1] = (uint8_t)(v >> 8);
}

static void put_u32(int off, uint32_t v)
{
    s_regs[off]     = (uint8_t)(v & 0xFF);
    s_regs[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    s_regs[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    s_regs[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ---------- 初始化 ---------- */
esp_err_t report_init(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[REPORT_REG_ID]     = REPORT_MAGIC;
    s_regs[REPORT_REG_ID + 1] = REPORT_FW_VERSION;
    s_regs[REPORT_REG_ID + 2] = REPORT_STATUS_BOOT;

    i2c_slave_config_t cfg = {
        .i2c_port        = 0,                        /* 端口 0（摄像头 SCCB 在端口 1） */
        .sda_io_num      = REPORT_I2C_SDA_GPIO,
        .scl_io_num      = REPORT_I2C_SCL_GPIO,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth  = 64,
        .slave_addr      = REPORT_I2C_ADDR,
        .addr_bit_len    = I2C_ADDR_BIT_LEN_7,
        .intr_priority   = 0,
        .flags.access_ram_en = true,                 /* 打开 RAM 寄存器模式 */
    };

    esp_err_t err = i2c_new_slave_device(&cfg, &s_slave);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_slave_device 失败: %s", esp_err_to_name(err));
        s_slave = NULL;
        return err;
    }

    err = report_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 I2C RAM 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C 从机就绪: 地址 0x%02X, SDA=IO%d, SCL=IO%d, 寄存器表 %d 字节",
             REPORT_I2C_ADDR, REPORT_I2C_SDA_GPIO, REPORT_I2C_SCL_GPIO, REPORT_REG_SIZE);
    return ESP_OK;
}

esp_err_t report_flush(void)
{
    if (s_slave == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 计算校验和（字节 3，自身除外），主机用它识别「撕裂读」 */
    uint8_t cs = 0;
    for (int i = 0; i < REPORT_REG_SIZE; i++) {
        if (i != REPORT_REG_CHECKSUM) {
            cs = (uint8_t)(cs + s_regs[i]);
        }
    }
    s_regs[REPORT_REG_CHECKSUM] = cs;

    return i2c_slave_write_ram(s_slave, 0, s_regs, REPORT_REG_SIZE);
}

/* ---------- 更新字段（更新后需调用 report_flush） ---------- */
void report_set_status(report_status_t st)
{
    s_regs[REPORT_REG_ID + 2] = (uint8_t)st;
}

void report_set_occluded(bool occluded)
{
    if (occluded) {
        s_regs[REPORT_REG_ID + 2] |= REPORT_FLAG_OCCLUDED;
    } else {
        s_regs[REPORT_REG_ID + 2] &= (uint8_t)~REPORT_FLAG_OCCLUDED;
    }
}

void report_set_detect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    put_u16(REPORT_REG_DETECT + 0, (uint16_t)x);
    put_u16(REPORT_REG_DETECT + 2, (uint16_t)y);
    put_u16(REPORT_REG_DETECT + 4, (uint16_t)w);
    put_u16(REPORT_REG_DETECT + 6, (uint16_t)h);
}

void report_set_env(uint16_t brightness, uint16_t motion)
{
    put_u16(REPORT_REG_ENV + 0, brightness);
    put_u16(REPORT_REG_ENV + 2, motion);
}

void report_set_frame_count(uint32_t count)
{
    put_u32(REPORT_REG_HEARTBEAT, count);
}
