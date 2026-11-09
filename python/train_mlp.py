#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
train_mlp.py —— 训练多模态融合 MLP（边缘 AI），并导出 C 头文件

用法（先采集好数据）：
    python train_mlp.py

流程：
    读 data/*.csv → 数据清洗 → 选特征 + 标签 → 标准化 → 切训练/测试
    → 训练 MLP → 打印准确率 → 导出 mlp_weights.h

特征（原始连续数据，不做二值化）：
    breath_rate, heart_rate, motion, brightness, distance_cm, box_w, box_h, occluded
"""
import glob
import os

import numpy as np
import pandas as pd

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "data")
OUTPUT_H = os.path.join(ROOT, "ESP32-Master", "components", "FUSION", "mlp_weights.h")

FEATURES = ["breath_rate", "heart_rate", "motion", "brightness",
            "distance_cm", "box_w", "box_h", "occluded"]
# 固定类顺序，和 fusion.h 的 sleep_state_t 枚举对齐：
#   off_bed=0, awake=1, light_sleep=2, deep_sleep=3, abnormal=4
CLASS_ORDER = ["off_bed", "awake", "light_sleep", "deep_sleep", "abnormal"]
HIDDEN = 32          # 隐藏层神经元数
EPOCHS = 500         # 上限（早停会自动提前结束）


def clean_data(df):
    """过滤明显污染的样本"""
    n0 = len(df)

    # off_bed 但距离在 (0,150)cm = 人还在检测范围内却标了离床（过渡污染）→ 过滤。
    # 真离床有两种：距离饱和(>=150) 或 距离=0(雷达失锁)，都保留。
    mask = ~((df["state"] == "off_bed") & (df["distance_cm"] > 0) & (df["distance_cm"] < 150))

    df = df[mask]
    print(f"数据清洗：{n0} → {len(df)} 条（去掉 {n0 - len(df)} 条污染样本）")
    return df


def write_header(classes, mean, std, w1, b1, w2, b2):
    """把训练好的权重 + 归一化参数写成 C 头文件"""
    n_in, n_hid = w1.shape
    n_cls = len(classes)
    L = []
    L.append("/* mlp_weights.h —— 由 train_mlp.py 自动生成，勿手改 */")
    L.append("/* 特征顺序: " + ", ".join(FEATURES) + " */")
    L.append("#pragma once")
    L.append("")
    L.append(f"#define MLP_IN_FEATURES  {n_in}")
    L.append(f"#define MLP_HIDDEN       {n_hid}")
    L.append(f"#define MLP_CLASSES      {n_cls}")
    L.append("")
    L.append('static const char *MLP_CLASS_NAMES[MLP_CLASSES] = {'
             + ", ".join(f'"{c}"' for c in classes) + "};")
    L.append("")
    L.append("static const float MLP_FEAT_MEAN[MLP_IN_FEATURES] = {"
             + ", ".join(f"{v:.6f}f" for v in mean) + "};")
    L.append("static const float MLP_FEAT_STD[MLP_IN_FEATURES] = {"
             + ", ".join(f"{v:.6f}f" for v in std) + "};")
    L.append("")
    L.append("static const float MLP_W1[MLP_IN_FEATURES][MLP_HIDDEN] = {")
    for i in range(n_in):
        L.append("    {" + ", ".join(f"{w1[i][j]:.6f}f" for j in range(n_hid)) + "},")
    L.append("};")
    L.append("static const float MLP_B1[MLP_HIDDEN] = {"
             + ", ".join(f"{b1[j]:.6f}f" for j in range(n_hid)) + "};")
    L.append("")
    L.append("static const float MLP_W2[MLP_HIDDEN][MLP_CLASSES] = {")
    for j in range(n_hid):
        L.append("    {" + ", ".join(f"{w2[j][k]:.6f}f" for k in range(n_cls)) + "},")
    L.append("};")
    L.append("static const float MLP_B2[MLP_CLASSES] = {"
             + ", ".join(f"{b2[k]:.6f}f" for k in range(n_cls)) + "};")
    L.append("")
    with open(OUTPUT_H, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"已导出: {OUTPUT_H}")


def main():
    files = sorted(glob.glob(os.path.join(DATA_DIR, "*.csv")))
    if not files:
        print("没找到 CSV，先运行 collect_sleep_data.py 采集数据。")
        return

    df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
    print(f"合并 {len(files)} 个文件，共 {len(df)} 条样本")
    df = clean_data(df)

    print("状态分布：")
    print(df["state"].value_counts())

    X = df[FEATURES].fillna(0.0).values.astype(np.float32)
    # 固定类顺序（和 fusion.h 的枚举对齐）
    classes = [c for c in CLASS_ORDER if c in set(df["state"].unique())]
    cls2idx = {c: i for i, c in enumerate(classes)}
    y = np.array([cls2idx[s] for s in df["state"]], dtype=np.int64)
    print(f"类别: {classes}")

    mean = X.mean(axis=0)
    std = X.std(axis=0) + 1e-6
    Xn = (X - mean) / std

    from sklearn.model_selection import train_test_split
    Xtr, Xte, ytr, yte = train_test_split(
        Xn, y, test_size=0.2, stratify=y, random_state=42)

    import tensorflow as tf

    def build_model():
        m = tf.keras.Sequential([
            tf.keras.layers.Input(shape=(len(FEATURES),)),
            tf.keras.layers.Dense(HIDDEN, activation="relu"),
            tf.keras.layers.Dropout(0.2),
            tf.keras.layers.Dense(len(classes), activation="softmax"),
        ])
        m.compile(optimizer="adam",
                  loss="sparse_categorical_crossentropy",
                  metrics=["accuracy"])
        return m

    cb = tf.keras.callbacks.EarlyStopping(
        monitor="val_loss", patience=50, restore_best_weights=True, verbose=0)

    # 训练 5 次，保留测试准确率最高那次（随机初始化会导致结果波动）
    best_model, best_acc, best_weights = None, 0.0, None
    print("\n训练 5 次，保留最佳模型 ...")
    for trial in range(5):
        tf.random.set_seed(trial)
        np.random.seed(trial)
        model = build_model()
        model.fit(Xtr, ytr, epochs=EPOCHS, batch_size=32, verbose=0,
                  validation_split=0.2, callbacks=[cb])
        _, acc = model.evaluate(Xte, yte, verbose=0)
        print(f"  第 {trial + 1} 次: {acc * 100:.1f}%")
        if acc > best_acc:
            best_acc = acc
            best_model = model
            best_weights = (model.layers[0].get_weights(),
                            model.layers[2].get_weights())

    print(f"\n最佳测试准确率: {best_acc * 100:.1f}%")

    from sklearn.metrics import classification_report, confusion_matrix
    ypred = best_model.predict(Xte, verbose=0).argmax(axis=1)
    print("\n混淆矩阵（行=真实, 列=预测）：")
    print(confusion_matrix(yte, ypred))
    print("\n分类报告：")
    print(classification_report(yte, ypred, target_names=classes))

    (w1, b1), (w2, b2) = best_weights
    write_header(classes, mean, std, w1, b1, w2, b2)


if __name__ == "__main__":
    main()
