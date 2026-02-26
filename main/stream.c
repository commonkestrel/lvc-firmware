#include <string.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_h264_types.h"
#include "driver/isp_core.h"
#include "stream.h"

static bool is_init = false;
static uint8_t *frame_buffer;
static uint32_t frame_length;

static const char *TAG = "lvc::stream.c";

static const esp_video_init_csi_config_t csi_config = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port = I2C_PORT,
            .scl_pin = SCL_PIN,
            .sda_pin = SDA_PIN,
        },
        .freq = I2C_FREQ,
    },
    .reset_pin = -1,
    .pwdn_pin = -1,
};

static const esp_video_init_config_t cam_config = {
    .csi = &csi_config,
};

esp_err_t stream_init(void) {
    if (is_init) {
        return ESP_OK;
    }

    const esp_video_init_config_t *cam_config_ptr = &cam_config;

    ESP_LOGI(TAG, "MIPI-CSI camera sensor I2C port=%d, scl_pin=%d, sda_pin=%d, freq=%d",
             I2C_PORT,
             SCL_PIN,
             SDA_PIN,
             I2C_FREQ);

    esp_err_t ret = esp_video_init(cam_config_ptr);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to initialize camera");

    is_init = true;

    return ESP_OK;
}

esp_err_t stream_log_capability(int fd, struct v4l2_capability *capability) {
    esp_err_t ret = ioctl(fd, VIDIOC_QUERYCAP, capability);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability->version >> 16),
             (uint8_t)(capability->version >> 8),
             (uint8_t)capability->version);
    ESP_LOGI(TAG, "driver:  %s", capability->driver);
    ESP_LOGI(TAG, "card:    %s", capability->card);
    ESP_LOGI(TAG, "bus:     %s", capability->bus_info);

    ESP_LOGI(TAG, "capabilities:");
    if (capability->capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
    }
    if (capability->capabilities & V4L2_CAP_READWRITE) {
        ESP_LOGI(TAG, "\tREADWRITE");
    }
    if (capability->capabilities & V4L2_CAP_ASYNCIO) {
        ESP_LOGI(TAG, "\tASYNCIO");
    }
    if (capability->capabilities & V4L2_CAP_STREAMING) {
        ESP_LOGI(TAG, "\tSTREAMING");
    }
    if (capability->capabilities & V4L2_CAP_META_OUTPUT) {
        ESP_LOGI(TAG, "\tMETA_OUTPUT");
    }
    if (capability->capabilities & V4L2_CAP_TIMEPERFRAME) {
        ESP_LOGI(TAG, "\tTIMEPERFRAME");
    }
    if (capability->capabilities & V4L2_CAP_DEVICE_CAPS) {
        ESP_LOGI(TAG, "device capabilities:");
        if (capability->device_caps & V4L2_CAP_VIDEO_CAPTURE) {
            ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
        }
        if (capability->device_caps & V4L2_CAP_READWRITE) {
            ESP_LOGI(TAG, "\tREADWRITE");
        }
        if (capability->device_caps & V4L2_CAP_ASYNCIO) {
            ESP_LOGI(TAG, "\tASYNCIO");
        }
        if (capability->device_caps & V4L2_CAP_STREAMING) {
            ESP_LOGI(TAG, "\tSTREAMING");
        }
        if (capability->device_caps & V4L2_CAP_META_OUTPUT) {
            ESP_LOGI(TAG, "\tMETA_OUTPUT");
        }
        if (capability->device_caps & V4L2_CAP_TIMEPERFRAME) {
            ESP_LOGI(TAG, "\tTIMEPERFRAME");
        }
    }

    return ret;
}

int stream_open(void) {
    struct v4l2_capability capability;

    const int fmt = V4L2_PIX_FMT_YUV420;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "failed to open camera - %d", fd);
        return -1;
    }

    int err = stream_log_capability(fd, &capability);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to get camera capabilities");
        return -1;
    }

        struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = WIDTH,
        .fmt.pix.height = HEIGHT,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420,
    };

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "failed to set format");
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "failed to require buffer");
        return ESP_FAIL;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to query buffer");
        return ESP_FAIL;
    }

    frame_buffer = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (!frame_buffer) {
        ESP_LOGE(TAG, "failed to map buffer");
        return ESP_FAIL;
    }

    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to queue video frame");
        return ESP_FAIL;
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "failed to start stream");
        return ESP_FAIL;
    }

    struct timeval time = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    if (ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &time) != 0) {
        ESP_LOGE(TAG, "failed to set timeout");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started stream");

    return fd;
}

esp_err_t stream_capture_frame(int fd, uint8_t *buffer, uint32_t *length) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    ESP_LOGI(TAG, "i am getting called i swear");

    if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to receive video frame");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "am i though?");

    esp_err_t ret;
    if (buf.flags & V4L2_BUF_FLAG_DONE) {
        *length = buf.bytesused;
        memcpy(frame_buffer, buffer, *length);
        ret = ESP_OK;
    } else {
        // Let the calling function know that a frame isn't ready yet
        ret = ESP_ERR_TIMEOUT;
    }

    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to queue video frame)");
        return ESP_FAIL;
    }

    return ret;
}
