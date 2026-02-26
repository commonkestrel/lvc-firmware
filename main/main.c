#include <stdio.h>
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "esp_flash.h"
#include "stream.h"
#include "multicast.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <sys/param.h>

static const char *TAG = "lvc::main.c";

static esp_netif_t *netif;
static TaskHandle_t multicast_task_handle;
static uint8_t *frame_buffer;
static uint32_t frame_length;
static int stream_fd;

static void multicast_task(void *pvParameter) {
    while (1) {
        int err = 0;
        int sock = create_multicast_socket(netif);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        while (err >= 0) {
            ESP_LOGI(TAG, "Multicast waiting for semaphore");
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            ESP_LOGI(TAG, "Sending packet!");
            err = send_multicast_packet(sock, frame_buffer, frame_length);
        }
        
        ESP_LOGE(TAG, "Error sending multicast packet");
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        close_socket(sock);
    }
}

void send_frame(uint8_t *buffer, uint32_t length) {
    ESP_LOGI(TAG, "Sent frame");
    frame_buffer = buffer;
    frame_length = length;
    xTaskNotifyGive(multicast_task_handle);
}

void encode_task(void *pvParameter) {
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.gop = 30;
    cfg.fps = 10;
    cfg.res.width = WIDTH;
    cfg.res.height = HEIGHT;

    cfg.rc.bitrate = CONFIG_H264_BITRATE;
    cfg.rc.qp_min = CONFIG_H264_MIN_QP;
    cfg.rc.qp_max = CONFIG_H264_MAX_QP;

    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;

    // Initialize encoder
    esp_h264_enc_t *enc = NULL;
    int err = esp_h264_enc_hw_new(&cfg, &enc);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to initialize new h264 encoder");
        return;
    }

    // Allocate input/output buffers
    int width = ((cfg.res.width +15) >> 4 << 4);
    int height = ((cfg.res.height+15) >> 4 << 4);


    esp_h264_enc_in_frame_t in_frame = {.raw_data.len = width * height + (width * height >> 1)};
    in_frame.raw_data.buffer = esp_h264_aligned_calloc(16, 1, in_frame.raw_data.len, &in_frame.raw_data.len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
                                                
    esp_h264_enc_out_frame_t out_frame = {.raw_data.len = in_frame.raw_data.len};
    out_frame.raw_data.buffer = esp_h264_aligned_calloc(16, 1, in_frame.raw_data.len, &in_frame.raw_data.len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);

    // Start encoding
    esp_h264_enc_open(enc);


    uint8_t *video_buffer;
    // Encoding loop
    while (1) {
        int err = stream_capture_frame(stream_fd, in_frame.raw_data.buffer, &in_frame.raw_data.len);
        if (err == ESP_ERR_TIMEOUT) {
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        } else if (err != ESP_OK) {
            break;
        }
        esp_h264_enc_process(enc, &in_frame, &out_frame);
        ESP_LOGI(TAG, "Sending frame");
        send_frame(out_frame.raw_data.buffer, out_frame.length);
        // stream_release_frame(stream_fd, &video_buffer)
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "aaaand it's over");

    // Resource release
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    esp_h264_free(in_frame.raw_data.buffer);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = stream_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video camera init failed");
        return;
    }

    stream_fd = stream_open();
    if (stream_fd < 0) {
        ESP_LOGE(TAG, "Failed to open camera stream");
        return;
    }

    xTaskCreate(&encode_task, "encode_task", 4096, NULL, 5, NULL);


    if (eth_connect(&netif) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize ethernet handle");
    }

    xTaskCreate(&multicast_task, "multicast_task", 4096, NULL, 5, &multicast_task_handle);
}
