# features 组件：图像特征提取（亮度/运动量）
# 依赖 esp32-camera：因为接口用到了 camera_fb_t 类型
idf_component_register(
    SRCS "features.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera
)
