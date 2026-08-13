#include "mpeg_ts.h"
#include "esp_err.h"
#include "multicast.h"
#include <stdint.h>
#include <sys/_timeval.h>
#include <sys/time.h>

// static const char *TAG = "lvc::mpeg_ts.c";

const async_crc_params_t crc_params = {
    .width = 32,
    .polynomial = 0x04C11DB7,
    .init_value = 0xFFFFFFFF,
    .final_xor_value = 0xFFFFFFFF,
    .reverse_input = true,
    .reverse_output = true,
};

const async_crc_config_t crc_config = {
    .backlog = 8,
    .dma_burst_size = 16,
};

uint64_t get_dts(void);
uint64_t get_pts(bool dts);
unsigned int write_pes_header(uint8_t *packet, int type, uint8_t *pes_cc);
esp_err_t send_ts_table(transport_stream_t *ts, uint8_t *table_data, uint8_t data_len, uint8_t *cc, uint16_t pid, uint16_t ident, uint8_t table_id);

esp_err_t create_ts(transport_stream_t *ts, esp_netif_t *netif) {
    ts->pes_cc = 0;
    ts->pat_cc = 0;
    ts->pmt_cc = 0;

    int sock = create_multicast_socket(netif);
    if (sock < 0) {
        return ESP_FAIL;
    }

    ts->multicast_socket = sock;
    ts->datagram = malloc(TS_UDP_DATAGRAM);
    ts->datagram_offset = 0;

    return esp_async_crc_install_gdma_ahb(&crc_config, &ts->crc_hdl);
}

inline void close_ts(transport_stream_t ts) {
    close_socket(ts.multicast_socket);
}

esp_err_t send_pes_packets(transport_stream_t *ts, const void *dataptr, size_t len) {
    uint8_t packet[TS_PACKET_LENGTH];
    const uint8_t *data = (const uint8_t *) dataptr;
    esp_err_t err = ESP_OK;

    int header_len = write_pes_header((uint8_t *) &packet, -len, &ts->pes_cc);
    int payload_len = TS_PACKET_LENGTH - header_len;

    bool breaking = header_len > 31;

    // The break condition is already covered by the ending packet check,
    // no need to look twice.
    for (;;) {
        for (int i = 0; i < payload_len; i++) {
            packet[header_len + i] = data[i];
        }

        for (int i = 0; i < TS_PACKET_LENGTH; i++) {
            ts->datagram[ts->datagram_offset + i] = packet[i];
        }
        
        ts->datagram_offset += TS_PACKET_LENGTH;
        if (ts->datagram_offset == TS_UDP_DATAGRAM) {    
            // Send TS packet to the multicast socket.
            int current_err = send_multicast_packet(ts->multicast_socket, (const void *) ts->datagram, TS_UDP_DATAGRAM);
            // It's better to try to send the rest of the packets on error,
            // as MPEG-TS is designed for unreliable connections.
            if (current_err != ESP_OK) {
                err = current_err;
            }

            ts->datagram_offset = 0;
        }

        if (breaking) {
            return err;
        }

        len -= payload_len;
        data += payload_len;

        // Not enough bytes for a full intermediate packet
        if (len < 184) {
            // Fill header with enough stuffing bytes so that the
            // end of the PES packet matches the end of the TS packet.
            header_len = write_pes_header((uint8_t *) &packet, len, &ts->pes_cc);
            payload_len = TS_PACKET_LENGTH - header_len;
            // Exit the function after the last PES packet is sent
            breaking = true;
        } else {
            header_len = write_pes_header((uint8_t *) &packet, PES_INTER, &ts->pes_cc);
            payload_len = TS_PACKET_LENGTH - header_len;
        }
    }
}

unsigned int write_pes_header(uint8_t *packet, int type, uint8_t *pes_cc) {
    unsigned int header_len;

    if (type < PES_INTER) { // Beginning PES packet
        packet[0] = 0x47;
        packet[1] = 0x40 + (TS_VIDEO_PID >> 8);
        packet[2] = TS_VIDEO_PID & 0xFF;
        packet[3] = 0x30 + (*pes_cc & 0x0F);

        int start = 6;

        if (-type < TS_PACKET_LENGTH - 12) {
            const int stuffing_len = TS_PACKET_LENGTH + type - 31;

            packet[4] = 1 + stuffing_len;
            packet[5] = 0x20;

            for (int i = 0; i < stuffing_len; i++) {
                packet[6+i] = 0xFF;
            }

            start += stuffing_len;
        } else {
            // One byte in the adaptation field, not including this one
            packet[4] = 1;
            // Denote an elementary stream.
            packet[5] = 0x40;
        }

        // int64_t pcr = get_current_pcr();

        // PES start code prefix
        packet[start] = 0x00;
        packet[start+1] = 0x00;
        packet[start+2] = 0x01;

        // PES stream id
        packet[start+3] = 0xE0;

        if (-type >= 1 << 16) {
            packet[start+4] = 0x00; // ((-type) & 0xFF00) >> 8;
            packet[start+5] = 0x00; // (-type) & 0xFF;
        } else {
            packet[start+4] = ((-type + 19) & 0xFF00) >> 8;
            packet[start+5] = (-type + 19) & 0xFF;
        }

        packet[start+6] = 0x81;
        packet[start+7] = 0xc0;
        packet[start+8] = 10;

        // Insert the Presentation TimeStamp
        uint64_t pts = get_pts(true);
        packet[start+9] = (pts >> 32) & 0xFF;
        packet[start+10] = (pts >> 24) & 0xFF;
        packet[start+11] = (pts >> 16) & 0xFF;
        packet[start+12] = (pts >> 8) & 0xFF;
        packet[start+13] = (pts >> 0) & 0xFF;

        uint64_t dts = get_dts();
        packet[start+14] = (dts >> 32) & 0xFF;
        packet[start+15] = (dts >> 24) & 0xFF;
        packet[start+16] = (dts >> 16) & 0xFF;
        packet[start+17] = (dts >> 8) & 0xFF;
        packet[start+18] = (dts >> 0) & 0xFF;

        // why the FUCK does ffmpeg add a whole NAL unit to the H264 stream
        packet[start+19] = 0x00;
        packet[start+20] = 0x00;
        packet[start+21] = 0x00;
        packet[start+22] = 0x01;
        packet[start+23] = 0x09;
        packet[start+24] = 0xF0;

        header_len = start + 25;
    } else if (type > PES_INTER) { // Ending PES packet
        packet[0] = 0x47;
        packet[1] = TS_VIDEO_PID >> 8;
        packet[2] = TS_VIDEO_PID & 0xFF;
        packet[3] = 0x30 + (*pes_cc & 0x0F);
        
        // One byte of flags and `stuffing_len` bytes of stuffing in adaptation field
        const int stuffing_len = TS_PACKET_LENGTH - type - 6;
        packet[4] = stuffing_len + 1;
        // Denote an elementary stream
        packet[5] = 0x40;


        for (int i = 0; i < stuffing_len; i++) {
            packet[6 + i] = 0xFF;
        }
            
        header_len = 6 + stuffing_len;
    } else { // Intermediate PES packet
        packet[0] = 0x47;
        packet[1] = TS_VIDEO_PID >> 8;
        packet[2] = TS_VIDEO_PID & 0xFF;
        packet[3] = 0x10 + (*pes_cc & 0x0F);

        header_len = 4;
    }

    *pes_cc += 1;
    return header_len;
}

esp_err_t send_pat(transport_stream_t *ts) {
    uint8_t section_data[4];

    // Program number 1 (the NIT takes up program 0)
    section_data[0] = 0x00;
    section_data[1] = 0x01;

    section_data[2] = 0xE0 + (TS_PMT_PID >> 8);
    section_data[3] = TS_PMT_PID & 0xFF;

    // PAT tables have a PID and table ID of 0.
    return send_ts_table(ts, (uint8_t *) &section_data, 4, &ts->pat_cc, 0x0000, 0x0000, 0x00);
}

esp_err_t send_pmt(transport_stream_t *ts) {
    uint8_t section_data[9];

    // Denote an unused PCR PID
    section_data[0] = 0xFF;
    section_data[1] = 0xFF;

    // No need for program descriptors, set the length to 0
    section_data[2] = 0xF0;
    section_data[3] = 0x00;

    // Denote an H.264 stream type
    section_data[4] = 0x1b;

    // Point to the elementary stream at the video pid
    section_data[5] = 0xE0 + (TS_VIDEO_PID >> 8);
    section_data[6] = TS_VIDEO_PID & 0xFF;

    // No need for elementary stream descriptors, set the length to 0
    section_data[7] = 0x00;
    section_data[8] = 0x00;

    return send_ts_table(ts, (uint8_t *) &section_data, 9, &ts->pmt_cc, TS_PMT_PID, 0, 2);
}

esp_err_t send_ts_table(transport_stream_t *ts, uint8_t *table_data, uint8_t data_len, uint8_t *cc, uint16_t pid, uint16_t ident, uint8_t table_id) {
    uint8_t buffer[TS_PACKET_LENGTH];

    // Write TS header
    buffer[0] = 0x47;
    buffer[1] = 0x40 + ((pid >> 8) & 0x1F);
    buffer[2] = pid & 0xFF;
    buffer[3] = 0x10 + (*cc & 0x0F);
    (*cc)++;

    // No need to skip any bytes.
    buffer[4] = 0;
    buffer[5] = table_id;
    buffer[6] = 0b10110000;
    buffer[7] = data_len + 5 + TS_CRC_LEN;

    // Section long header
    buffer[8] = ident >> 8;
    buffer[9] = ident & 0xFF;

    // Table version 0, to be used now
    buffer[10] = 0b11000001;
    // Since we have a single stream,
    // there will only be one section in both our PAT and PMT
    buffer[11] = 0;
    buffer[12] = 0;

    for (int i = 0; i < data_len; i++) {
        buffer[i + TS_TABLE_HEADER_LEN] = table_data[i];
    }

    for (int i = TS_TABLE_HEADER_LEN + data_len + TS_CRC_LEN; i < TS_PACKET_LENGTH; i++) {
        buffer[i] = 0xFF;
    }

    ESP_ERROR_CHECK(esp_crc_calc_blocking(
        ts->crc_hdl, 
        &buffer,
        TS_TABLE_HEADER_LEN + data_len,
        &crc_params,
        -1,
        (uint32_t *)(&buffer + TS_TABLE_HEADER_LEN + data_len)
    ));

    return send_multicast_packet(ts->multicast_socket, &buffer, TS_PACKET_LENGTH);
}

uint64_t get_pts(bool dts) {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);    
    
    // Offset the PTS by 2ms to allow for any dropped frames to be decoded
    int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    int64_t time_pts = time_us * 90000 / 1000000;

    // some bullshit tbh, i have no idea why the PTS and DTS are laid out like this
    int64_t pts = (dts ? 0xC0000 : 0x80000) + 0b00000100000000000000010000000000000001;
    pts += time_pts & 0x0000007FFF;
    time_pts = time_pts << 1;
    pts += time_pts & 0x007FFF0000;
    time_pts = time_pts << 1;
    pts += time_pts & 0x3800000000;

    return pts;
}

uint64_t get_dts(void) {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    int64_t time_dts = time_us * 90000 / 1000000;

    // some bullshit tbh, i have no idea why the dts and DTS are laid out like this
    int64_t dts = 0b01000100000000000000010000000000000001;
    dts += time_dts & 0x0000007FFF;
    time_dts = time_dts << 1;
    dts += time_dts & 0x007FFF0000;
    time_dts = time_dts << 1;
    dts += time_dts & 0x3800000000;

    return dts;
}
