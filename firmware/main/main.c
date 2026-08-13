#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_ap.h"
#include "http_server.h"
#include "can_twai.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Moto Guzzi CAN Toolkit & Diagnostic Firmware   ");
    ESP_LOGI(TAG, "  Target: XIAO ESP32-C3 / C6 + SN65HVD230        ");
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Wi-Fi SoftAP & Captive Portal
    ESP_LOGI(TAG, "Starting Wi-Fi Access Point...");
    wifi_init_softap();

    // 3. Initialize HTTP & WebSockets Server
    ESP_LOGI(TAG, "Starting WebServer...");
    start_web_server();

    // 4. Initialize TWAI CAN Bus Driver (500 kbps)
    ESP_LOGI(TAG, "Starting TWAI CAN Driver...");
    guzzi_can_init();

    ESP_LOGI(TAG, "System initialization complete!");
    ESP_LOGI(TAG, "Connect to Wi-Fi 'Guzzi-CAN-Tool' (Password: 12345678) and open http://192.168.4.1/");
}
