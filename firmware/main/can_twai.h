#ifndef CAN_TWAI_H
#define CAN_TWAI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pins for Seeed Studio XIAO ESP32-C3 / C6 connected to SN65HVD230
#define CAN_TX_GPIO 5
#define CAN_RX_GPIO 4

/**
 * Initializes the ESP32 TWAI (CAN bus) driver at 500 kbps.
 */
esp_err_t guzzi_can_init(void);

/**
 * Executes active UDS Diagnostic Service Reset on Dashboard ECU (0x720).
 * Clears the service wrench / key light on the TFT cluster.
 */
esp_err_t guzzi_can_reset_service_wrench(void);

/**
 * Executes active UDS Clear Fault Codes (DTCs) on Engine ECU (0x7E0).
 */
esp_err_t guzzi_can_clear_ecu_dtc(void);

#ifdef __cplusplus
}
#endif

#endif // CAN_TWAI_H
