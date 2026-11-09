# 基于毫米波雷达与视觉多模态融合的婴儿睡眠安全监测系统

> AIoT 嵌入式系统创新大赛 · 边缘智能（多模态融合 + 异常检测）方向

## 项目简介

面向婴儿睡眠安全监测场景，采用**毫米波雷达（HLK-LD6002）** 非接触式监测**呼吸、心率、距离**，结合**摄像头（GC2145）** 提取**运动量、亮度、遮挡、运动区域**等视觉特征，将两类异构信号在**端侧部署的轻量级神经网络（MLP）** 中融合，实时判断婴儿睡眠状态（离床 / 清醒 / 浅睡 / 深睡），并对**呼吸异常**进行规则告警。

**所有 AI 推理在 ESP32-S3 上本地完成**，模型仅 1.7KB、单次推理约 27 微秒，无需 AI 框架运行时，不依赖云端。

## 系统架构

```
雷达(LD6002) ──UART──┐
                     ├──► 主控 ESP32-S3 ──WiFi/MQTT──► PC(Mosquitto/上位机)
摄像头(S3-Cam) ──I2C──┘        │
                                └──► OLED(本地显示)
```

- **主控**：多模态融合推理（手写 MLP 前向传播）+ 呼吸异常检测 + WiFi/MQTT
- **从机**：图像特征提取（亮度/运动/遮挡/运动框）
- **上位机**：MQTT Broker + 数据采集 + 模型训练

## 目录结构

```
├── ESP32-Master/          # 主控固件（ESP-IDF，融合推理 + MQTT + OLED）
├── ESP32-Camera/          # 从机固件（ESP-IDF，图像特征提取）
├── python/                # 上位机脚本
│   ├── collect_sleep_data.py   # 数据采集（MQTT 订阅 + 人工打标）
│   ├── train_mlp.py            # 模型训练（导出 C 头文件）
│   ├── broker_responder.py     # UDP 广播 Broker 自动发现
│   └── ld6002_parse.py         # 雷达帧解析（调试）
├── data/                  # 采集的训练数据（CSV）
└── .gitignore
```

## 硬件清单

| 部件 | 型号 | 作用 |
|---|---|---|
| 主控 | ESP32-S3（N8R8） | 融合推理 + 通信 |
| 视觉从机 | ESP32-S3-Cam（GC2145） | 图像特征 |
| 雷达 | HLK-LD6002 | 呼吸/心率/距离 |
| 显示 | OLED SSD1306 128×64 | 本地显示 |

## 快速开始

### 1. 编译烧录固件

```bash
# 主控（连接雷达 IO9/IO10、从机 IO17/IO18、OLED IO7/IO6）
cd ESP32-Master && idf.py build && idf.py -p COM9 flash

# 从机（IO47/IO48 作为 I2C 从机）
cd ESP32-Camera && idf.py build && idf.py -p COM4 flash
```

### 2. 启动上位机

```bash
pip install paho-mqtt pandas tensorflow scikit-learn
python broker_responder.py      # 启动 Broker 自动发现
# 另开 Mosquitto（MQTT Broker，端口 1883）
```

### 3. 采集数据 + 训练模型

```bash
python collect_sleep_data.py    # 按 1~5 键人工打标，生成 CSV
python train_mlp.py             # 训练 MLP，导出 ESP32-Master/components/FUSION/mlp_weights.h
```

## 关键指标

| 指标 | 数值 |
|---|---|
| AI 模型 | MLP 8→32→4，436 参数 |
| 模型大小 | 1744 字节（≈1.7KB） |
| 单次推理 | 平均 27 微秒 |
| 测试准确率 | 73.9%（993 条样本，4 类） |
| 呼吸异常检测 | 医学阈值 + 10 秒去抖（规则，不依赖训练数据） |
| 通信 | UART + I2C 从机 + WiFi/MQTT + UDP 自动发现 |

## 讲解视频

- 作品讲解与实物演示视频：见大赛提交表单中的视频链接（上传至 B 站 / 抖音等平台）
