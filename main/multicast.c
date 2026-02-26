#include "multicast.h"
#include "esp_log.h"
#include "esp_event.h"
#include <netdb.h>

static const char *TAG = "lvc::multicast.c";
#define MULTICAST_IPV4 CONFIG_MULTICAST_IPV4_ADDR
#define MULTICAST_PORT CONFIG_MULTICAST_PORT

static SemaphoreHandle_t got_ip = NULL;

static struct sockaddr_in sdest;

static void eth_on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
    xSemaphoreGive(got_ip);
}

static void eth_on_conn(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "Ethernet Link Up!");
}

esp_err_t eth_init(esp_eth_handle_t *eth_handle_out) {
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = -1; // i have no clue what the physical address is, i'd like to find it automatically
    phy_config.reset_gpio_num = 51; // ESP32p4's pin

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    esp32_emac_config.smi_gpio.mdc_num = 31; // ESP32p4's pin
    esp32_emac_config.smi_mdio_gpio_num = 52; //ESP32p4's number

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "create MAC instance failed");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "create PHY instance failed");
        return ESP_FAIL;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);

    if (esp_eth_driver_install(&config, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        mac->del(mac);
        phy->del(phy);
        return ESP_FAIL;
    }

    *eth_handle_out = eth_handle;

    return ESP_OK;
}

esp_err_t eth_connect(esp_netif_t **netif_out) {
    esp_eth_handle_t eth_handle = NULL;
    if (eth_init(&eth_handle) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();

    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handle);

    if (esp_netif_attach(eth_netif, eth_netif_glue)) {
        ESP_LOGE(TAG, "failed to attach ethernet to TCP/IP stack");
        return ESP_FAIL;
    }

    got_ip = xSemaphoreCreateBinary();
    if (got_ip == NULL) {
        ESP_LOGE(TAG, "out of memory!");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &eth_on_conn, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Waiting for IP.");

    #ifdef CONFIG_STATIC_IP
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(esp_netif_ip_info_t));
        ip.ip.addr = ipaddr_addr(CONFIG_DEVICE_IPV4_ADDR);
        ip.netmask.addr = ipaddr_addr(CONFIG_DEVICE_IPV4_NETMASK);
        ip.gw.addr = ipaddr_addr(CONFIG_DEVICE_IPV4_GATEWAY);

        esp_netif_dhcpc_stop(eth_netif);
        if (esp_netif_set_ip_info(eth_netif, &ip)) {
            ESP_LOGE(TAG, "Failed to set static IP info");
            return ESP_FAIL;
        }
    #endif

    xSemaphoreTake(got_ip, portMAX_DELAY);

    *netif_out = eth_netif;

    // set destination multicast addresses for sending from these sockets
    sdest.sin_family = PF_INET,
    sdest.sin_port = htons(CONFIG_MULTICAST_PORT),
    // We know this inet_aton will pass because we did it above already
    inet_aton(CONFIG_MULTICAST_IPV4_ADDR, &sdest.sin_addr.s_addr);

    return ESP_OK;
}


static int socket_add_multicast_group(esp_netif_t *netif, int sock) {
    struct ip_mreq imreq = { 0 };
    struct in_addr inaddr = { 0 };
    int err = 0;

    esp_netif_ip_info_t ip_info = { 0 };
    err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address info. Error 0x%x", err);
        return err;
    }
    inet_addr_from_ip4addr(&inaddr, &ip_info.ip);

    // Configure multicast address to listen to
    err = inet_aton(CONFIG_MULTICAST_IPV4_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV4 multicast address '%s' is invalid.", CONFIG_MULTICAST_IPV4_ADDR);
        return -1;
    }

    ESP_LOGI(TAG, "Configured IPV4 multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will most likely result in an error.", CONFIG_MULTICAST_IPV4_ADDR);
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
    }

    return err;
}

int create_multicast_socket(esp_netif_t *netif) {
    struct sockaddr_in saddr = { 0 };
    int err = 0;

    int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to initialize socket. Error %d", errno);
        return -1;
    }

    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(CONFIG_MULTICAST_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        close(sock);
        return -1;
    }

    uint8_t ttl = CONFIG_MULTICAST_TTL;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        close(sock);
        return -1;
    }

    err = socket_add_multicast_group(netif, sock);
    if (err < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

esp_err_t send_multicast_packet(int sock, const void *dataptr, size_t len) {
    char addrbuf[32] = { 0 };

    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_socktype = SOCK_DGRAM,
        .ai_family = AF_INET,
    };

    struct addrinfo *res;
    int err = getaddrinfo(CONFIG_MULTICAST_IPV4_ADDR, NULL, &hints, &res);
    if (err < 0) {
        ESP_LOGE(TAG, "getaddrinfo() failed for IPV4 destination address. errno: %d", err);
        return ESP_FAIL;
    }
    if (res == 0) {
        ESP_LOGE(TAG, "getaddrinfo() did not return any addresses");
        return ESP_FAIL;
    }

    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(CONFIG_MULTICAST_PORT);
    inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, addrbuf, sizeof(addrbuf)-1);
    ESP_LOGI(TAG, "Sending to IPV4 multicast address %s:%d...", addrbuf, CONFIG_MULTICAST_PORT);

    err = sendto(sock, dataptr, len, 0, res->ai_addr, res->ai_addrlen);
    ESP_LOGI(TAG, "Send UDP packet to %s", CONFIG_MULTICAST_IPV4_ADDR);
    freeaddrinfo(res);
    if(err < 0) {
        ESP_LOGE(TAG, "IPV4 sendto failed. errno: %d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void close_socket(int sock) {
    shutdown(sock, 0);
    close(sock);
}
