#ifndef MULTICAST_H
#define MULTICAST_H

#include "esp_netif.h"
#include "esp_err.h"
#include "esp_eth.h"

#define PACKET_SIZE 2056

esp_err_t send_multicast_packet(int sock, const void *dataptr, size_t len);
int create_multicast_socket(esp_netif_t *netif);
esp_err_t eth_connect(esp_netif_t **netif_out);
void close_socket(int sock);

#endif