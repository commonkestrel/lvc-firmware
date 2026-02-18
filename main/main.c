#include <stdio.h>
#include "esp_flash.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"

bool capture_frame(void *buffer) {
    return true;
}

void send_packet(uint8_t *buffer) {

}

void app_main(void)
{
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.gop = 30;
    cfg.fps = 30;
    cfg.res.width = 640;
    cfg.res.height = 480;

    cfg.rc.bitrate = (640 * 480 * 30) / 100;    
    cfg.rc.qp_min = 26;
    cfg.rc.qp_max = 30;

    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;

    // Initialize encoder
    esp_h264_enc_t *enc = NULL;
    esp_h264_enc_hw_new(&cfg, &enc);

    // Allocate input/output buffers
    esp_h264_enc_in_frame_t in_frame = {.raw_data.len = 640 * 480 * 1.5};
    in_frame.raw_data.buffer = esp_h264_aligned_calloc(128, 1, 
                                                    in_frame.raw_data.len, 
                                                    &in_frame.raw_data.len, 
                                                    ESP_H264_MEM_INTERNAL);
                                                
    esp_h264_enc_out_frame_t out_frame = {.raw_data.len = 640*480*1.5};
    out_frame.raw_data.buffer = esp_h264_aligned_calloc(128, 1, out_frame.raw_data.len, &out_frame.raw_data.len, ESP_H264_MEM_INTERNAL);

    // Start encoding
    esp_h264_enc_open(enc);

    // Encoding loop
    while (capture_frame(in_frame.raw_data.buffer)) {
        esp_h264_enc_process(enc, &in_frame, &out_frame);
        send_packet(out_frame.raw_data.buffer);
    }

    // Resource release
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    esp_h264_free(in_frame.raw_data.buffer);
}
