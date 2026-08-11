#ifndef MPEG_TS_H
#define MPEG_TS_H

#include "esp_err.h"
#include "esp_netif.h"

#define TS_PACKET_LENGTH (188)
#define PES_INTER (0)

struct transport_stream_obj {
    int multicast_socket;
    int sequence;
    // int64_t base_pcr_us;
};

typedef struct transport_stream_obj transport_stream_t;

esp_err_t create_ts(transport_stream_t *ts, esp_netif_t *netif);
void close_ts(transport_stream_t ts);
esp_err_t send_pes_packets(transport_stream_t *ts, const void *dataptr, size_t len);

#endif