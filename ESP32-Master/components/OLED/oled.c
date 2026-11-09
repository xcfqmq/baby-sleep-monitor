/*
 * oled.c —— SSD1306 128x64 OLED 驱动（I2C）
 *
 * 最小实现：I2C 初始化 + SSD1306 初始化序列 + 1 页 framebuffer + 5x7 字体。
 * 只包含本项目需要的字符（数字、大写字母、少量符号），减少代码体积。
 */
#include "oled.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "debug.h"

static const char *TAG = "OLED";

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_fb[OLED_WIDTH * OLED_LINES];   /* 128 * 8 = 1024 字节 */

/* ---------------- 5x7 字体（列优先，bit0=顶部） ----------------
 * 顺序：空格 / 0-9 / A-Z / : . - _ /
 */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
};

/* 字符 → 字体数组下标；未知字符当空格 */
static int char_index(char c)
{
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    switch (c) {
    case ' ': return 0;
    case ':': return 37;
    case '.': return 38;
    case '-': return 39;
    case '_': return 40;
    case '/': return 41;
    default:  return 0;
    }
}

/* ---------------- I2C 底层 ---------------- */
static void cmd(uint8_t b)
{
    uint8_t buf[2] = { 0x00, b };   /* 控制字节 0x00 = 命令 */
    i2c_master_transmit(s_dev, buf, 2, -1);
}

static void data(const uint8_t *d, size_t len)
{
    uint8_t buf[1 + 128];           /* 0x40 控制字节 + 最多 128 列 */
    buf[0] = 0x40;                  /* 0x40 = 数据 */
    memcpy(buf + 1, d, len);
    i2c_master_transmit(s_dev, buf, len + 1, -1);
}

/* ---------------- 初始化 ---------------- */
esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = -1,                       /* 自动挑空闲控制器 */
        .scl_io_num = OLED_I2C_SCL_GPIO,
        .sda_io_num = OLED_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        D_LOGE(TAG, "I2C 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 诊断：扫描总线上有哪些设备（定位地址用） */
    D_LOGI(TAG, "扫描 I2C 总线 (SDA=IO%d SCL=IO%d)...", OLED_I2C_SDA_GPIO, OLED_I2C_SCL_GPIO);
    int found = 0;
    for (int addr = 1; addr < 128; addr++) {
        if (i2c_master_probe(bus, (uint16_t)addr, 50) == ESP_OK) {
            D_LOGI(TAG, "  发现设备: 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        D_LOGW(TAG, "  总线上没发现任何设备！检查 VCC/GND 供电、SDA/SCL 是否接反");
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        D_LOGE(TAG, "OLED 设备挂载失败（地址 0x%02X）: %s",
                 OLED_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    /* SSD1306 初始化序列（128x64） */
    cmd(0xAE);          /* display off */
    cmd(0xD5); cmd(0x80);   /* clock div */
    cmd(0xA8); cmd(0x3F);   /* multiplex 64 */
    cmd(0xD3); cmd(0x00);   /* offset */
    cmd(0x40);              /* start line 0 */
    cmd(0x8D); cmd(0x14);   /* charge pump on */
    cmd(0x20); cmd(0x00);   /* 水平寻址 */
    cmd(0xA1);              /* segment remap */
    cmd(0xC8);              /* COM 扫描方向 */
    cmd(0xDA); cmd(0x12);   /* COM 引脚 */
    cmd(0x81); cmd(0xCF);   /* 对比度 */
    cmd(0xD9); cmd(0xF1);   /* 预充电 */
    cmd(0xDB); cmd(0x40);   /* VCOM */
    cmd(0xA4);              /* 正常显示 */
    cmd(0xA6);              /* 非反色 */
    cmd(0xAF);              /* display on */

    oled_clear();
    oled_flush();
    D_LOGI(TAG, "OLED 就绪: SDA=IO%d SCL=IO%d 地址 0x%02X",
             OLED_I2C_SDA_GPIO, OLED_I2C_SCL_GPIO, OLED_I2C_ADDR);
    return ESP_OK;
}

/* ---------------- 绘图 ---------------- */
void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_pixel(int x, int y, int on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    int page = y / 8;
    int bit  = y % 8;
    if (on) {
        s_fb[x + page * OLED_WIDTH] |= (uint8_t)(1 << bit);
    } else {
        s_fb[x + page * OLED_WIDTH] &= (uint8_t)~(1 << bit);
    }
}

static void fb_char(int x, int y, char c)
{
    const uint8_t *g = font5x7[char_index(c)];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            fb_pixel(x + col, y + row, (g[col] >> row) & 1);
        }
    }
}

void oled_text(int line, const char *text)
{
    if (line < 0 || line >= OLED_LINES) {
        return;
    }
    int y = line * 8;
    int x = 0;
    for (const char *p = text; *p && x + 6 <= OLED_WIDTH; p++) {
        fb_char(x, y, *p);
        x += 6;                          /* 5px 字宽 + 1px 间距 */
    }
}

void oled_flush(void)
{
    for (int page = 0; page < OLED_LINES; page++) {
        cmd((uint8_t)(0xB0 + page));     /* 设置页地址 */
        cmd(0x00);                       /* 列低字节 */
        cmd(0x10);                       /* 列高字节 */
        data(&s_fb[page * OLED_WIDTH], OLED_WIDTH);
    }
}

void oled_show_status(bool wifi_ok, bool mqtt_ok, const char *ip,
                      const char *state, float breath_rate, float heart_rate,
                      float distance_cm)
{
    char line[32];
    char state_upper[16];
    oled_clear();

    /* OLED 字体只支持大写字母，把小写状态（off_bed 等）转大写再显示 */
    if (state) {
        int i;
        for (i = 0; state[i] && i < (int)sizeof(state_upper) - 1; i++) {
            char ch = state[i];
            state_upper[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
        }
        state_upper[i] = '\0';
        state = state_upper;
    }

    snprintf(line, sizeof(line), "WIFI:%s", wifi_ok ? "OK" : "NG");
    oled_text(0, line);

    snprintf(line, sizeof(line), "MQTT:%s", mqtt_ok ? "OK" : "NG");
    oled_text(1, line);

    snprintf(line, sizeof(line), "%s", ip ? ip : "IP:--");
    oled_text(2, line);

    snprintf(line, sizeof(line), "ST:%s", state ? state : "--");
    oled_text(3, line);

    snprintf(line, sizeof(line), "BR:%.0f HR:%.0f", breath_rate, heart_rate);
    oled_text(4, line);

    snprintf(line, sizeof(line), "DST:%.0f", distance_cm);
    oled_text(5, line);

    oled_flush();
}
