#include "dns_server.h"
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DNS_SERVER";

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static int dns_socket = -1;
static TaskHandle_t dns_task_handle = NULL;
static volatile bool dns_running = false;

static int parse_dns_name(const uint8_t *buffer, int offset, int max_len, char *name_out, int name_max) {
    int name_len = 0;
    int curr = offset;
    
    while (curr < max_len) {
        uint8_t len = buffer[curr];
        if (len == 0) {
            curr++; // Skip terminating zero
            break;
        }
        
        // Handle compression pointer (unsupported in query names)
        if ((len & 0xC0) == 0xC0) {
            return -1;
        }
        
        if (curr + 1 + len > max_len) {
            return -1; // Overflow
        }
        
        if (name_len > 0 && name_len < name_max - 1) {
            name_out[name_len++] = '.';
        }
        
        for (int i = 0; i < len && name_len < name_max - 1; i++) {
            name_out[name_len++] = (char)buffer[curr + 1 + i];
        }
        
        curr += 1 + len;
    }
    
    if (name_len < name_max) {
        name_out[name_len] = '\0';
    } else {
        name_out[name_max - 1] = '\0';
    }
    
    return curr; // Returns offset after the name
}

static const char* allowed_dns_domains[] = {
    "guzzi.by",
    "www.guzzi.by",
    "guzzi.local",
    "connectivitycheck.gstatic.com",
    "connectivitycheck.android.com",
    "clients3.google.com",
    "captive.apple.com",
    "www.msftconnecttest.com",
    "www.msftncsi.com"
};

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);
    
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    int opt = 1;
    setsockopt(dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    int err = bind(dns_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "DNS socket unable to bind: errno %d", errno);
        close(dns_socket);
        dns_socket = -1;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS Captive Portal Server listening on port 53 (Selective Android/iOS filtering active)...");
    dns_running = true;
    
    while (dns_running) {
        int len = recvfrom(dns_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &addr_len);
        if (len < 0) {
            if (dns_running) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            }
            break;
        }
        
        if (len < sizeof(dns_header_t)) {
            continue;
        }
        
        dns_header_t *header = (dns_header_t *)rx_buffer;
        uint16_t qd_count = ntohs(header->qd_count);
        
        if (qd_count != 1) {
            continue; // Only support single question queries
        }
        
        char domain_name[128];
        int offset = parse_dns_name(rx_buffer, sizeof(dns_header_t), len, domain_name, sizeof(domain_name));
        if (offset < 0 || offset + 4 > len) {
            ESP_LOGW(TAG, "Failed to parse query domain name");
            continue;
        }
        
        bool should_resolve = false;
        for (size_t i = 0; i < sizeof(allowed_dns_domains) / sizeof(allowed_dns_domains[0]); i++) {
            if (strcasecmp(domain_name, allowed_dns_domains[i]) == 0) {
                should_resolve = true;
                break;
            }
        }
        
        // Ignore un-allowed background app DNS queries to prevent Android request spam
        if (!should_resolve) {
            continue;
        }
        
        ESP_LOGI(TAG, "DNS Captive Query resolved: %s -> 192.168.4.1", domain_name);
        
        int q_len = offset + 4; // Header + question (QTYPE + QCLASS)
        uint8_t tx_buffer[512];
        memcpy(tx_buffer, rx_buffer, q_len); // Copy question verbatim
        
        dns_header_t *resp_header = (dns_header_t *)tx_buffer;
        resp_header->flags = htons(0x8580); // Response, Authoritative, Recursion Available
        resp_header->an_count = htons(1);
        resp_header->ns_count = 0;
        resp_header->ar_count = 0;
        
        uint8_t answer[] = {
            0xC0, 0x0C,            // Pointer to domain name in question
            0x00, 0x01,            // Type A (IPv4)
            0x00, 0x01,            // Class IN
            0x00, 0x00, 0x00, 0x3C, // TTL 60 seconds
            0x00, 0x04,            // RDLENGTH 4 bytes
            192, 168, 4, 1         // IP 192.168.4.1
        };
        
        if (q_len + sizeof(answer) <= sizeof(tx_buffer)) {
            memcpy(tx_buffer + q_len, answer, sizeof(answer));
            int resp_len = q_len + sizeof(answer);
            sendto(dns_socket, tx_buffer, resp_len, 0, (struct sockaddr *)&source_addr, addr_len);
        }
    }
    
    if (dns_socket >= 0) {
        close(dns_socket);
        dns_socket = -1;
    }
    ESP_LOGI(TAG, "DNS Server task exiting.");
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t start_dns_server(void) {
    if (dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS server is already running");
        return ESP_OK;
    }
    if (xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void stop_dns_server(void) {
    if (dns_task_handle == NULL) {
        return;
    }
    dns_running = false;
    if (dns_socket >= 0) {
        shutdown(dns_socket, SHUT_RDWR);
        close(dns_socket);
        dns_socket = -1;
    }
}
