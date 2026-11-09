# report 组件：把检测结果通过 I2C 从机（0x52）上报给主机
idf_component_register(
    SRCS "report.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2c
)
