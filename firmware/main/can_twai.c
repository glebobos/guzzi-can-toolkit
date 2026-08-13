#include "can_twai.h"
#include "guzzi_dbc.h"
#include <string.h>
#include "esp_log.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GUZZI_CAN";
static TaskHandle_t twai_rx_task_handle = NULL;

static void twai_rx_task(void *pvParameters) {
    twai_message_t rx_msg;
    ESP_LOGI(TAG, "TWAI CAN RX Task started listening at 500 kbps");

    while (1) {
        if (twai_receive(&rx_msg, pdMS_TO_TICKS(100)) == ESP_OK) {
            if (!(rx_msg.rtr) && !(rx_msg.extd)) {
                guzzi_dbc_parse_frame(rx_msg.identifier, rx_msg.data, rx_msg.data_length_code);
            }
        }
    }
}

esp_err_t guzzi_can_init(void) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TWAI CAN driver initialized successfully (TX: GPIO %d, RX: GPIO %d, 500kbps)", CAN_TX_GPIO, CAN_RX_GPIO);

    xTaskCreate(twai_rx_task, "twai_rx", 4096, NULL, 10, &twai_rx_task_handle);
    return ESP_OK;
}

esp_err_t guzzi_can_reset_service_wrench(void) {
    ESP_LOGI(TAG, "Executing UDS Service Wrench Reset on Dashboard ECU (0x720)...");

    // 1. Extended Diagnostic Session Request: 0x720 [ 02 10 03 00 00 00 00 00 ]
    twai_message_t tx_sess = {
        .identifier = 0x720,
        .data_length_code = 8,
        .data = {0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}
    };
    twai_transmit(&tx_sess, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Start Service Reset Routine: 0x720 [ 04 31 01 02 01 00 00 00 ]
    twai_message_t tx_reset = {
        .identifier = 0x720,
        .data_length_code = 8,
        .data = {0x04, 0x31, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00}
    };
    esp_err_t res = twai_transmit(&tx_reset, pdMS_TO_TICKS(500));
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "UDS Service Wrench Reset frame transmitted to 0x720");
    }
    return res;
}

esp_err_t guzzi_can_clear_ecu_dtc(void) {
    ESP_LOGI(TAG, "Executing UDS Clear DTC Fault Codes on Engine ECU (0x7E0)...");

    // 1. Extended Session: 0x7E0 [ 02 10 03 00 00 00 00 00 ]
    twai_message_t tx_sess = {
        .identifier = 0x7E0,
        .data_length_code = 8,
        .data = {0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}
    };
    twai_transmit(&tx_sess, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Clear Diagnostic Info: 0x7E0 [ 04 14 FF FF FF 00 00 00 ]
    twai_message_t tx_clear = {
        .identifier = 0x7E0,
        .data_length_code = 8,
        .data = {0x04, 0x14, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00}
    };
    esp_err_t res = twai_transmit(&tx_clear, pdMS_TO_TICKS(500));
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "UDS Clear DTC frame transmitted to 0x7E0");
    }
    return res;
}
