#include "sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "bmi270_config_file.h"
#include <string.h>

static const char *TAG = "SENSOR";

/* ======================= 接线配置 ======================= */
#define I2C_SDA_GPIO        5       /* SDA 接到 GPIO5 */
#define I2C_SCL_GPIO        4       /* SCL 接到 GPIO4 */
#define I2C_FREQ_HZ         400000  /* 400 kHz */
#define BMI270_I2C_ADDR     0x68    /* SDO 接 GND → 地址 0x68 */

/* ======================= BMI270 寄存器 ======================= */
#define BMI270_CHIP_ID          0x00
#define BMI270_ACC_X_LSB        0x0C   /* 加速度数据（6 字节） */
#define BMI270_GYR_X_LSB        0x12   /* 陀螺仪数据（6 字节） */
#define BMI270_INTERNAL_STATUS  0x21   /* 配置文件加载状态 */
#define BMI270_ACC_CONF         0x40   /* 加速度 ODR/带宽 */
#define BMI270_ACC_RANGE        0x41   /* 加速度量程 */
#define BMI270_GYR_CONF         0x42   /* 陀螺仪 ODR/带宽 */
#define BMI270_GYR_RANGE        0x43   /* 陀螺仪量程 */
#define BMI270_INIT_CTRL        0x59   /* 配置加载控制 */
#define BMI270_INIT_ADDR_0      0x5B   /* 配置地址高字节 */
#define BMI270_INIT_ADDR_1      0x5C   /* 配置地址低字节 */
#define BMI270_INIT_DATA        0x5E   /* 配置数据 */
#define BMI270_PWR_CONF         0x7C   /* 省电模式配置（APS） */
#define BMI270_PWR_CTRL         0x7D   /* 传感器使能 */
#define BMI270_CMD              0x7E   /* 命令寄存器 */

#define BMI270_CHIP_ID_EXPECTED 0x24
#define BMI270_SOFT_RESET       0xB6

static i2c_master_dev_handle_t s_dev = NULL;
static uint32_t                s_seq = 0;
static bool                    s_ok  = false;

/* ---------------- 底层 I2C 读写 ---------------- */

/* 写寄存器：reg 后跟数据（BMI270 是 8 位寄存器地址） */
static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[64];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    return reg_write(reg, &val, 1);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

/* ---------------- 初始化 ---------------- */

void SENSOR_init(void)
{
    /* 1. 创建 I2C 总线 */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                         /* 自动选择端口 */
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 打开内部上拉（外部也建议有 4.7k） */
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* 2. 在总线上挂载 BMI270 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMI270_I2C_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    /* 3. 读芯片 ID 验证（若失败多半是接线/地址/供电问题） */
    uint8_t chip_id = 0;
    if (reg_read(BMI270_CHIP_ID, &chip_id, 1) != ESP_OK || chip_id != BMI270_CHIP_ID_EXPECTED) {
        ESP_LOGE(TAG, "BMI270 芯片 ID 读取失败 (0x%02x, 期望 0x24)。检查 SDA/SCL 接线、3.3V 供电、地址", chip_id);
        return;
    }
    ESP_LOGI(TAG, "BMI270 检测成功 (chip id = 0x24)");

    /* 4. 软复位 */
    ESP_ERROR_CHECK(reg_write8(BMI270_CMD, BMI270_SOFT_RESET));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 5. 关闭高级省电模式（APS）：复位后 APS 默认开启，开着时配置加载会失败 */
    reg_write8(BMI270_PWR_CONF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(2));

    /* 6. 上传配置：一次性连续写入 INIT_DATA，不设置 INIT_ADDR（地址自动递增）
     *    参考官方例程的做法（ESP32_BMI270/BMI270.cpp） */
    ESP_ERROR_CHECK(reg_write8(BMI270_INIT_CTRL, 0x00));
    {
        static uint8_t cfg_buf[BMI270_CONFIG_FILE_SIZE + 1];
        cfg_buf[0] = BMI270_INIT_DATA;
        memcpy(&cfg_buf[1], bmi270_config_file, BMI270_CONFIG_FILE_SIZE);
        ESP_ERROR_CHECK(i2c_master_transmit(s_dev, cfg_buf, BMI270_CONFIG_FILE_SIZE + 1, 2000));
    }
    ESP_ERROR_CHECK(reg_write8(BMI270_INIT_CTRL, 0x01));
    vTaskDelay(pdMS_TO_TICKS(40));

    /* 7. 检查加载状态：低 4 位 == 1 表示成功 */
    uint8_t status = 0;
    reg_read(BMI270_INTERNAL_STATUS, &status, 1);
    if ((status & 0x0F) != 0x01) {
        ESP_LOGE(TAG, "BMI270 配置文件加载失败 (internal_status=0x%02x)", status);
        return;
    }
    ESP_LOGI(TAG, "BMI270 配置文件加载成功");

    /* 7. 配置加速度计：ODR 100Hz、带宽 normal、量程 ±4g */
    uint8_t acc[2] = { 0x08 | (0x02 << 4), 0x01 };   /* ACC_CONF | ACC_RANGE */
    ESP_ERROR_CHECK(reg_write(BMI270_ACC_CONF, acc, 2));

    /* 8. 配置陀螺仪：ODR 100Hz、带宽 normal、量程 ±2000dps */
    uint8_t gyr[2] = { 0x08 | (0x02 << 4), 0x00 };   /* GYR_CONF | GYR_RANGE */
    ESP_ERROR_CHECK(reg_write(BMI270_GYR_CONF, gyr, 2));

    /* 9. 使能加速度计 + 陀螺仪 + 温度 (bit2|bit1|bit3 = 0x0E) */
    ESP_ERROR_CHECK(reg_write8(BMI270_PWR_CTRL, 0x04 | 0x02 | 0x08));

    s_ok = true;
    ESP_LOGI(TAG, "BMI270 初始化完成，开始输出六轴数据");
}

/* ---------------- 读取 ---------------- */

void SENSOR_read(imu_sample_t *s)
{
    s->t_us = esp_timer_get_time();

    if (!s_ok) {
        /* 传感器没初始化成功时，输出全 0，避免垃圾数据 */
        s->seq = 0;
        memset(s->accel, 0, sizeof(s->accel));
        memset(s->gyro, 0, sizeof(s->gyro));
        return;
    }

    uint8_t data[12];
    if (reg_read(BMI270_ACC_X_LSB, data, 12) != ESP_OK) {
        s->seq = 0;
        memset(s->accel, 0, sizeof(s->accel));
        memset(s->gyro, 0, sizeof(s->gyro));
        return;
    }

    /* 小端 16 位：先低字节后高字节 */
    int16_t ax = (int16_t)((data[1]  << 8) | data[0]);
    int16_t ay = (int16_t)((data[3]  << 8) | data[2]);
    int16_t az = (int16_t)((data[5]  << 8) | data[4]);
    int16_t gx = (int16_t)((data[7]  << 8) | data[6]);
    int16_t gy = (int16_t)((data[9]  << 8) | data[8]);
    int16_t gz = (int16_t)((data[11] << 8) | data[10]);

    s->seq      = s_seq++;
    s->accel[0] = ax / 8192.0f;   /* ±4g  ：32768 / 4 = 8192 LSB/g */
    s->accel[1] = ay / 8192.0f;
    s->accel[2] = az / 8192.0f;
    s->gyro[0]  = gx / 16.4f;     /* ±2000dps：32768 / 2000 = 16.384 LSB/dps */
    s->gyro[1]  = gy / 16.4f;
    s->gyro[2]  = gz / 16.4f;
}
