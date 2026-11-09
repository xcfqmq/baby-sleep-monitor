set(src_dirs
            LED
            WIFI
            SENSOR
            MQTT
            CAMLINK
            RADAR
            BROKER_DISC
            FUSION
            OLED)

set(include_dirs
            LED
            WIFI
            SENSOR
            MQTT
            CAMLINK
            RADAR
            BROKER_DISC
            FUSION
            COMMON
            OLED)

set(requires
            driver
            esp_wifi
            esp_netif
            esp_event
            esp_timer
            mqtt
            json
            esp_driver_i2c
            esp_driver_uart)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
