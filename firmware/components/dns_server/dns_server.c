#include "dns_server.h"
#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "DNS_SERVER";
static int dns_socket = -1;
static TaskHandle_t dns_task_handle = NULL;

#define DNS_PORT 53

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_server_task(void *pvParameters) {
    uint8_t buffer[512];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(dns_socket);
        dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive Portal Server started on port 53");

    while (1) {
        int len = recvfrom(dns_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < (int)sizeof(dns_header_t)) continue;

        dns_header_t *header = (dns_header_t *)buffer;
        header->flags = htons(0x8400); // Standard query response, No error
        header->an_count = header->qd_count; // Answer count = Query count

        int ptr = sizeof(dns_header_t);
        // Skip query name
        while (ptr < len && buffer[ptr] != 0) {
            ptr += buffer[ptr] + 1;
        }
        ptr += 5; // Skip null byte, QTYPE, and QCLASS

        if (ptr + 16 <= (int)sizeof(buffer)) {
            // Append Answer: PTR to Query Name (0xC00C), Type A (1), Class IN (1), TTL 60s (60), RDLENGTH 4, IP 192.168.4.1
            buffer[ptr++] = 0xC0;
            buffer[ptr++] = 0x0C;
            buffer[ptr++] = 0x00; // Type A
            buffer[ptr++] = 0x01;
            buffer[ptr++] = 0x00; // Class IN
            buffer[ptr++] = 0x01;
            buffer[ptr++] = 0x00; // TTL 60s
            buffer[ptr++] = 0x00;
            buffer[ptr++] = 0x00;
            buffer[ptr++] = 0x3C;
            buffer[ptr++] = 0x00; // RDLENGTH 4
            buffer[ptr++] = 0x04;
            buffer[ptr++] = 192;  // 192.168.4.1
            buffer[ptr++] = 168;
            buffer[ptr++] = 4;
            buffer[ptr++] = 1;

            sendto(dns_socket, buffer, ptr, 0, (struct sockaddr *)&client_addr, client_addr_len);
        }
    }
}

esp_err_t start_dns_server(void) {
    if (xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 5, &dns_task_handle) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void stop_dns_server(void) {
    if (dns_socket >= 0) {
        close(dns_socket);
        dns_socket = -1;
    }
    if (dns_task_handle != NULL) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
}
