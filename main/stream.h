#ifndef STREAM_H
#define STREAM_H
#include "esp_err.h"

#define DEVICE_NAME "/dev/video0"
#define I2C_FREQ 100000
#define I2C_PORT 0
#define SCL_PIN 8
#define SDA_PIN 7

#define HEIGHT CONFIG_CAMERA_HEIGHT
#define WIDTH CONFIG_CAMERA_WIDTH

esp_err_t stream_init(void);
esp_err_t stream_open(void);
esp_err_t stream_capture_frame(int fd, uint8_t *buffer, uint32_t *length);

#endif