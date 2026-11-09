# photo 组件 —— 摄像头（ESP32-S3-Cam / GC2145）

## 它负责什么

把摄像头的**初始化、取帧、归还帧、信息打印**封装起来。上层不关心引脚和驱动细节。

```
main.c  ──►  photo_init()       初始化（引脚/格式/PSRAM 都在组件里配好）
            photo_get_frame()   取一帧
            photo_return_frame() 归还一帧（必须调用！）
            photo_print_info()  打印传感器型号
```

## 板子引脚（来源：厂商例程 camera_setting.h 的 ESP32S3_EYE 段）

| 信号 | GPIO | 说明 |
|---|---|---|
| XCLK | 15 | 主时钟，驱动输出 15MHz |
| SIOD / SIOC | 4 / 5 | **SCCB（类 I2C）**，只用于配置寄存器，不是图像通路 |
| D0–D7 | 11, 9, 8, 10, 12, 18, 17, 16 | DVP 并口数据（图像走这里） |
| VSYNC / HREF / PCLK | 6 / 7 / 13 | 并口同步与时钟 |
| PWDN / RESET | -1 / -1 | 本板未引出，用软件复位 |

> 图像数据走 **DVP 并口**（速率由 PCLK 决定，约 15MHz），
> SIOD/SIOC 那条 I2C 只传寄存器配置（100~400kHz）——所以「I2C 太慢传不了图像」是误解。

## 默认配置

| 项 | 默认值 | 说明 |
|---|---|---|
| 分辨率 | 240×240 | 端侧 AI 推理用的尺寸 |
| 像素格式 | RGB565 | AI 用；要看图/传图改成 `PIXFORMAT_JPEG` |
| 帧缓冲 | 2 个，放 PSRAM | 连续采集；**必须开 PSRAM** |
| XCLK | 15 MHz | 厂商实测值 |

改配置的方式：`photo_init(&my_cfg)`，或先 `photo_default_config()` 再改字段。

## 依赖与前置条件

- 驱动：`components/esp32-camera/`（官方 esp32-camera，已随工程放入）
- `sdkconfig.defaults` 已开启 PSRAM（`CONFIG_SPIRAM` + `SPIRAM_MODE_OCT` + 80MHz）
- 构建时组件管理器会从乐鑫组件库拉取 `esp_jpeg`（需要联网）

## 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 日志 `PSRAM ID read error` / 启动重启 | PSRAM 模式不对：把 `CONFIG_SPIRAM_MODE_OCT` 改成 `CONFIG_SPIRAM_MODE_QUAD` |
| `esp_camera_init 失败: ESP_ERR_NOT_FOUND` | 排线没插紧、或摄像头没被识别（SCCB 读不到 PID） |
| 抓帧一直返回 NULL | 帧缓冲耗尽：检查是否每帧都调用了 `photo_return_frame` |
| 图像很暗/发绿 | 传感器型号与设置不匹配；先看 `photo_print_info()` 打出的 PID |
