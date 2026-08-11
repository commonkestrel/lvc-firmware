#include "mpeg_ts.h"
#include "esp_log.h"
#include "multicast.h"
#include <stdint.h>
#include <sys/_timeval.h>
#include <sys/time.h>

static const char *TAG = "lvc::mpeg_ts.c";

// int64_t get_current_pcr();
unsigned int write_pes_header(uint8_t *packet, int type, int *sequence);

esp_err_t create_ts(transport_stream_t *ts, esp_netif_t *netif) {
    ts->sequence = 0;

    int sock = create_multicast_socket(netif);
    if (sock < 0) {
        return ESP_FAIL;
    }

    ts->multicast_socket = sock;
    // ts->base_pcr = get_current_pcr();

    return ESP_OK;
}

inline void close_ts(transport_stream_t ts) {
    close_socket(ts.multicast_socket);
}

esp_err_t send_pes_packets(transport_stream_t *ts, const void *dataptr, size_t len) {
    uint8_t packet[TS_PACKET_LENGTH];
    const uint8_t *data = (const uint8_t *) dataptr;
    esp_err_t err = ESP_OK;

    int header_len = write_pes_header((uint8_t *) &packet, -len, &ts->sequence);
    int payload_len = TS_PACKET_LENGTH - header_len;

    bool breaking = header_len > 12;

    // The break condition is already covered by the ending packet check,
    // no need to look twice.
    for (;;) {
        for (int i = 0; i < payload_len; i++) {
            packet[header_len + i] = data[i];
        }

        // Send TS packet to the multicast socket.
        int current_err = send_multicast_packet(ts->multicast_socket, (const void *) &packet, TS_PACKET_LENGTH);
        // It's better to try to send the rest of the packets on error,
        // as MPEG-TS is designed for unreliable connections.
        if (current_err != ESP_OK) {
            err = current_err;
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
            header_len = write_pes_header((uint8_t *) &packet, len, &ts->sequence);
            payload_len = TS_PACKET_LENGTH - header_len;
            // Exit the function after the last PES packet is sent
            breaking = true;
        } else {
            header_len = write_pes_header((uint8_t *) &packet, PES_INTER, &ts->sequence);
            payload_len = TS_PACKET_LENGTH - header_len;
        }
    }
}

unsigned int write_pes_header(uint8_t *packet, int type, int *sequence) {
    unsigned int header_len;

    if (type < PES_INTER) { // Beginning PES packet
        packet[0] = 0x47;
        packet[1] = 0x41;
        packet[2] = 0xF5;
        packet[3] = 0x30 + (*sequence & 0x0F);

        int start = 6;

        if (-type < TS_PACKET_LENGTH - 12) {
            const int stuffing_len = TS_PACKET_LENGTH + type - 12;

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
            packet[5] = 0x20;
        }

        // int64_t pcr = get_current_pcr();

        // PES start code prefix
        packet[start] = 0x00;
        packet[start+1] = 0x00;
        packet[start+2] = 0x01;

        // PES stream id
        packet[start+3] = 0xE0;

        packet[start+4] = ((-type) & 0xFF00) >> 8;
        packet[start+5] = (-type) & 0xFF;

        header_len = start + 6;
    } else if (type > PES_INTER) { // Ending PES packet
        packet[0] = 0x47;
        packet[1] = 0x01;
        packet[2] = 0xF5;
        packet[3] = 0x30 + (*sequence & 0x0F);

        // One byte of flags and `type` bytes of stuffing in adaptation field
        packet[4] = type + 1;
        // Denote an elementary stream
        packet[5] = 0x20;

        for (int i = 0; i < type; i++) {
            packet[6 + i] = 0xFF;
        }
            
        header_len = 6 + type;
    } else { // Intermediate PES packet
        packet[0] = 0x47;
        packet[1] = 0x01;
        packet[2] = 0xF5;
        packet[3] = 0x10 + (*sequence & 0x0F);

        header_len = 4;
    }

    *sequence += 1;
    return header_len;
}

int64_t get_current_us() {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    return time_us;
}

// int64_t us_to_pcr(int64_t us) {
    
// }
