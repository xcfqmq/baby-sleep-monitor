# photo 组件：摄像头驱动封装
#
# 依赖说明：
#   REQUIRES esp32-camera —— photo.h 里 include 了 "esp_camera.h"，属于公开依赖
#   PRIV_REQUIRES driver   —— 仅供 photo.c 内部使用（GPIO/LEDC 等底层类型）
#
# 注意：本工程是「每个组件目录各自一个 CMakeLists.txt」的标准布局，
#       不要在上层 components/ 目录再放 CMakeLists.txt，否则 IDF 会把整个
#       components 目录当成单个组件、不再扫描子目录。

idf_component_register(
    SRCS "photo.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera
    PRIV_REQUIRES driver esp_timer
)
