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
 * Calculates genuine Magneti Marelli UDS SecurityAccess Key from a 4-byte Seed
 * using the Marelli Wichmann-Hill PRNG / Schrage algorithm with constants 0x1759 and 0x1215.
 */
void guzzi_marelli_calc_key(const uint8_t seed[4], uint8_t key[4]);

/**
 * Executes active UDS Diagnostic Service Reset on COBO Dashboard (0x7A0)
 * and Marelli 11MP Engine ECU (0x7E0).
 * Writes a full JSON report of sent frames and captured replies into `out_json`.
 */
esp_err_t guzzi_can_reset_service_wrench(char *out_json, size_t out_len);

/**
 * Executes active UDS Clear Fault Codes (DTCs) on Engine ECU (0x7E0).
 */
esp_err_t guzzi_can_clear_ecu_dtc(void);

/**
 * Read-only connectivity test: sends OBD Mode 01 PID 00 and UDS TesterPresent to
 * ECU/Dash targets, reports whether each frame was ACKed and lists any diagnostic replies.
 * Writes a JSON report into `out`.
 */
esp_err_t guzzi_can_ping(char *out, size_t out_len);
esp_err_t guzzi_can_probe_dash(char *out, size_t out_len);
esp_err_t guzzi_can_test_security_key(int cand_idx, const char *custom_key_hex, char *out, size_t out_len);

/**
 * Full bus diagnostic scanner: sweeps standard 11-bit diagnostic IDs (0x700 - 0x7EF)
 * and key 29-bit UDS targets to discover all active ECUs and Dashboards on the motorcycle.
 * Writes a JSON report into `out`.
 */
esp_err_t guzzi_can_scan_bus(char *out, size_t out_len);

/**
 * Hardware test: holds CTX LOW (static dominant) for hold_ms so the transceiver's differential
 * drive can be measured with a multimeter. Refuses to run while the bus is active (ignition ON).
 */
esp_err_t guzzi_can_dominant_test(uint32_t hold_ms);
esp_err_t guzzi_can_toggle_test(uint32_t total_ms);

#ifdef __cplusplus
}
#endif

#endif // CAN_TWAI_H
