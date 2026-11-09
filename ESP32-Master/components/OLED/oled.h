/*
 * oled.h —— SSD1306 OLED（0.96" 128x64）显示网络状态 + 睡眠信息
 *
 * 接线（主机 ESP32-Master）：
 *   OLED SCL ── GPIO8
 *   OLED SDA ── GPIO7
 *   OLED VCC ── 3.3V
 *   OLED GND ── GND
 *
 * 说明：用第二条 I2C 控制器（CAMLINK 占了一条，BMI270 已弃用，这里自动挑空闲的）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_I2C_SDA_GPIO  7
#define OLED_I2C_SCL_GPIO  6
#define OLED_I2C_ADDR      0x3C   /* 常见 0x3C；部分板是 0x3D */
#define OLED_WIDTH         128
#define OLED_HEIGHT        64
#define OLED_LINES         8      /* 每行 8px 高，共 8 行 */

/* 初始化 I2C 总线 + SSD1306，并清屏 */
esp_err_t oled_init(void);

/* 清屏（只清 framebuffer，不刷新到屏幕） */
void oled_clear(void);

/* 在第 line 行（0~7）写字（5x7 字体，自动截断超宽部分） */
void oled_text(int line, const char *text);

/* 把 framebuffer 刷到屏幕（改完字后调用一次） */
void oled_flush(void);

/* 便捷：一次性显示网络 + 睡眠状态（内部会 clear + flush） */
void oled_show_status(bool wifi_ok, bool mqtt_ok, const char *ip,
                      const char *state, float breath_rate, float heart_rate,
                      float distance_cm);

#ifdef __cplusplus
}
#endif
