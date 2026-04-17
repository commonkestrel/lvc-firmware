#include <stdio.h>
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "stream.h"
#include "multicast.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <sys/param.h>

static const char *TAG = "lvc::main.c";

static esp_netif_t *netif;
static int stream_fd;

void encode_task(void *pvParameter) {
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.gop = 15;
    cfg.fps = 15;
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
    esp_h264_enc_in_frame_t in_frame = {.raw_data.len = BUFFER_LEN};
    in_frame.raw_data.buffer = esp_h264_aligned_calloc(16, 1, in_frame.raw_data.len, &in_frame.raw_data.len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
                                                
    esp_h264_enc_out_frame_t out_frame = {.raw_data.len = in_frame.raw_data.len};
    out_frame.raw_data.buffer = esp_h264_aligned_calloc(16, 1, in_frame.raw_data.len, &in_frame.raw_data.len, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);

    // Start encoding
    esp_h264_enc_open(enc);

    int frame_count = 0;
    bool set_frame_rate = false;
    int64_t start_time_us = esp_timer_get_time();

    uint8_t *video_buffer;
    // Encoding loop
    while (1) {
        int err = 0;
        int sock = create_multicast_socket(netif);
        while (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
            vTaskDelay(5 / portTICK_PERIOD_MS);
            sock = create_multicast_socket(netif);
        }
        
        while (err >= 0) {
            int frame_count = 0;

            while (err >= 0) {
                //if (esp_timer_get_time() - start_time_us < 66667) {
                //    continue;
                //}

                //start_time_us = esp_timer_get_time();

                err = stream_capture_frame(stream_fd, in_frame.raw_data.buffer, &in_frame.raw_data.len);
                if (err == ESP_ERR_TIMEOUT) {
                    vTaskDelay(5 / portTICK_PERIOD_MS);
                    err = ESP_OK;
                    continue;
                } else if (err != ESP_OK) {
                    break;
                }

                frame_count++;

                // if (!set_frame_rate && esp_timer_get_time() - start_time_us >= (4 * 1000 * 1000)) {
                //     esp_h264_enc_param_hw_handle_t fps_hd;
                //     esp_h264_enc_hw_get_param_hd(enc, &fps_hd);
                //     esp_h264_enc_set_fps(&fps_hd->base, frame_count / 4);
                // }

                if (esp_h264_enc_process(enc, &in_frame, &out_frame) != ESP_H264_ERR_OK) {
                    ESP_LOGE(TAG, "error while encoding frame");
                    continue;
                }

                ESP_LOGI(TAG, "Sending packet of %d bytes", out_frame.length);
                send_multicast_packet(sock, out_frame.raw_data.buffer, out_frame.length);

                //if (frame_count > 30) {
                //    break;
               // }
            }

            //ESP_LOGI(TAG, "Reloading encoder!");
            //esp_h264_enc_close(enc);
            //esp_h264_enc_del(enc);
            //int err = esp_h264_enc_hw_new(&cfg, &enc);
            //esp_h264_enc_open(enc);
        }
        
        ESP_LOGE(TAG, "Error sending multicast packet, closing socket and restarting");
        close_socket(sock);
        // stream_release_frame(stream_fd, &video_buffer)
    }

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

    if (eth_connect(&netif) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize ethernet handle");
    }

    xTaskCreate(&encode_task, "encode_task", 4096, NULL, 5, NULL);
}
