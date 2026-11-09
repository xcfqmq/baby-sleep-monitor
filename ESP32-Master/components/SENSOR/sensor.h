#ifndef SENSOR_H_
#define SENSOR_H_

#include <stdint.h>

/*
 * 一帧 IMU 采样：六轴原始数据
 * 单位：accel = g（重力加速度的倍数），gyro = dps（度/秒）
 *
 * TODO(BMI270)：以后接上真实 BMI270 后，保持这个结构体不变，
 * 只改 sensor.c 里的 SENSOR_init()/SENSOR_read() 实现，调用方（main.c）无需改动。
 */
typedef struct {
    uint32_t seq;      /* 采样序号，从 0 递增 */
    int64_t  t_us;     /* 时间戳，微秒（esp_timer_get_time()） */
    float    accel[3]; /* ax ay az，单位 g */
    float    gyro[3];  /* gx gy gz，单位 dps */
} imu_sample_t;

void SENSOR_init(void);
void SENSOR_read(imu_sample_t *s);

#endif
