#include "http_server.h"
#include "guzzi_dbc.h"
#include "can_twai.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HTTP_SERVER";
static httpd_handle_t server = NULL;
static TaskHandle_t ws_broadcast_task_handle = NULL;

// File handler for SPIFFS web assets
static esp_err_t spiffs_file_handler(httpd_req_t *req) {
    char filepath[600];
    const char *uri = req->uri;

    // Captive portal redirects
    if (strcmp(uri, "/generate_204") == 0 ||
        strcmp(uri, "/canonical.html") == 0 ||
        strcmp(uri, "/connecttest.txt") == 0 ||
        strcmp(uri, "/redirect") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strcmp(uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html.gz");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%s.gz", uri);
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        // Fallback without .gz
        if (strcmp(uri, "/") == 0) {
            snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
        } else {
            snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);
        }
        if (stat(filepath, &st) != 0) {
            // Fallback to index for SPA client routing
            snprintf(filepath, sizeof(filepath), "/spiffs/index.html.gz");
            if (stat(filepath, &st) != 0) {
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        }
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (strstr(filepath, ".gz") != NULL) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    if (strstr(filepath, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    } else if (strstr(filepath, ".svg")) {
        httpd_resp_set_type(req, "image/svg+xml");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    }

    char chunk[1024];
    size_t chunk_size;
    while ((chunk_size = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunk_size) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// REST API: GET /api/telemetry
static esp_err_t api_telemetry_handler(httpd_req_t *req) {
    char json_buf[512];
    guzzi_telemetry_to_json(json_buf, sizeof(json_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_buf);
    return ESP_OK;
}

// REST API: POST /api/service/reset
static esp_err_t api_service_reset_handler(httpd_req_t *req) {
    esp_err_t res = guzzi_can_reset_service_wrench();
    httpd_resp_set_type(req, "application/json");
    if (res == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Service wrench reset frame sent\"}");
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

// REST API: POST /api/ecu/clear-dtc
static esp_err_t api_clear_dtc_handler(httpd_req_t *req) {
    esp_err_t res = guzzi_can_clear_ecu_dtc();
    httpd_resp_set_type(req, "application/json");
    if (res == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Clear DTC frame sent to ECU\"}");
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

// WebSockets /ws handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client handshake connected");
        return ESP_OK;
    }
    return ESP_OK;
}

// Broadcast task pushing telemetry to connected WebSocket clients at 10 Hz
static void ws_broadcast_task(void *pvParameters) {
    char json_buf[512];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz

        if (server == NULL) continue;

        size_t clients = 10;
        int client_fds[10];
        bool formatted = false;

        if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK && clients > 0) {
            for (size_t i = 0; i < clients; i++) {
                if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    if (!formatted) {
                        guzzi_telemetry_to_json(json_buf, sizeof(json_buf));
                        formatted = true;
                    }
                    httpd_ws_frame_t ws_pkt;
                    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                    ws_pkt.payload = (uint8_t *)json_buf;
                    ws_pkt.len = strlen(json_buf);
                    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

                    httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
                }
            }
        }
    }
}

esp_err_t start_web_server(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS partition...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&conf);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register API endpoints
        httpd_uri_t uri_telemetry = { .uri = "/api/telemetry", .method = HTTP_GET, .handler = api_telemetry_handler };
        httpd_register_uri_handler(server, &uri_telemetry);

        httpd_uri_t uri_service_reset = { .uri = "/api/service/reset", .method = HTTP_POST, .handler = api_service_reset_handler };
        httpd_register_uri_handler(server, &uri_service_reset);

        httpd_uri_t uri_clear_dtc = { .uri = "/api/ecu/clear-dtc", .method = HTTP_POST, .handler = api_clear_dtc_handler };
        httpd_register_uri_handler(server, &uri_clear_dtc);

        httpd_uri_t uri_ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
        httpd_register_uri_handler(server, &uri_ws);

        // Catch-all file handler for SPIFFS Web UI & Captive Portal
        httpd_uri_t uri_spiffs = { .uri = "/*", .method = HTTP_GET, .handler = spiffs_file_handler };
        httpd_register_uri_handler(server, &uri_spiffs);

        xTaskCreate(ws_broadcast_task, "ws_broadcast", 4096, NULL, 5, &ws_broadcast_task_handle);

        ESP_LOGI(TAG, "HTTP & WebSocket Server running on port 80");
        return ESP_OK;
    }
    return ESP_FAIL;
}

void stop_web_server(void) {
    if (ws_broadcast_task_handle != NULL) {
        vTaskDelete(ws_broadcast_task_handle);
        ws_broadcast_task_handle = NULL;
    }
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
