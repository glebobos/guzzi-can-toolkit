#include "can_twai.h"
#include "guzzi_dbc.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "GUZZI_CAN";
static TaskHandle_t twai_rx_task_handle = NULL;

// Capture buffer for diagnostic responses (filled by RX task while a ping is running)
#define DIAG_CAP_MAX 128
typedef struct {
    uint32_t t_ms;
    uint32_t id;
    bool extd;
    uint8_t dlc;
    uint8_t data[8];
} diag_cap_t;
static diag_cap_t s_cap[DIAG_CAP_MAX];
static volatile int s_cap_n = 0;
static volatile bool s_cap_on = false;
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_last_rx_ms = 0;
static volatile bool s_twai_paused = false;

static void log_twai_status(const char *stage) {
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        const char *state_str = "UNKNOWN";
        switch (status.state) {
            case TWAI_STATE_STOPPED:    state_str = "STOPPED"; break;
            case TWAI_STATE_RUNNING:    state_str = "RUNNING"; break;
            case TWAI_STATE_BUS_OFF:    state_str = "BUS_OFF"; break;
            case TWAI_STATE_RECOVERING: state_str = "RECOVERING"; break;
        }
        ESP_LOGI(TAG, "[%s] State: %s | TEC: %" PRIu32 " | REC: %" PRIu32 " | BusErr: %" PRIu32 " | TxQ: %" PRIu32 " | TxFails: %" PRIu32 " | RxQ: %" PRIu32 " | RxMissed: %" PRIu32 " | RxOverrun: %" PRIu32 " | ArbLost: %" PRIu32,
                 stage, state_str, status.tx_error_counter, status.rx_error_counter,
                 status.bus_error_count, status.msgs_to_tx, status.tx_failed_count, status.msgs_to_rx,
                 status.rx_missed_count, status.rx_overrun_count, status.arb_lost_count);
    } else {
        ESP_LOGE(TAG, "[%s] Unable to retrieve TWAI status", stage);
    }
}

static void twai_rx_task(void *pvParameters) {
    twai_message_t rx_msg;
    ESP_LOGI(TAG, "TWAI CAN RX Task started listening at 500 kbps");
    uint32_t rx_count = 0;

    while (1) {
        uint32_t alerts = 0;
        if (twai_read_alerts(&alerts, 0) == ESP_OK && alerts != 0) {
            if (alerts & TWAI_ALERT_BUS_OFF) {
                ESP_LOGE(TAG, ">>> ALERT: TWAI entered BUS-OFF state! Initiating recovery...");
                esp_err_t rec_err = twai_initiate_recovery();
                if (rec_err == ESP_OK) {
                    ESP_LOGI(TAG, "TWAI recovery initiated");
                } else {
                    ESP_LOGE(TAG, "Failed to initiate TWAI recovery: %s", esp_err_to_name(rec_err));
                }
            }
            if (alerts & TWAI_ALERT_BUS_RECOVERED) {
                ESP_LOGI(TAG, ">>> ALERT: TWAI bus recovered! Restarting TWAI driver...");
                esp_err_t start_err = twai_start();
                if (start_err == ESP_OK) {
                    ESP_LOGI(TAG, "TWAI driver restarted and running");
                } else {
                    ESP_LOGE(TAG, "Failed to restart TWAI driver: %s", esp_err_to_name(start_err));
                }
            }
            if (alerts & TWAI_ALERT_BUS_ERROR) {
                ESP_LOGW(TAG, ">>> ALERT: TWAI Bus Error detected (bit error / no ACK / stuffing)");
            }
            // TX success is reported synchronously by send_diag_frame() (keeps the RX task fast)
            if (alerts & TWAI_ALERT_TX_FAILED) {
                ESP_LOGW(TAG, ">>> ALERT: TWAI TX failed");
            }
            if (alerts & TWAI_ALERT_ERR_PASS) {
                ESP_LOGW(TAG, ">>> ALERT: TWAI entered Error Passive state");
            }
        }

        if (s_twai_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (twai_receive(&rx_msg, pdMS_TO_TICKS(100)) == ESP_OK) {
            rx_count++;
            s_last_rx_ms = esp_log_timestamp();
            if (rx_count == 1 || (rx_count % 500) == 0) {
                ESP_LOGI(TAG, "CAN RX active: %" PRIu32 " frames received (last ID: 0x%03" PRIX32 ")", rx_count, rx_msg.identifier);
            }

            // Diagnostic / UDS range: 11-bit (0x700 - 0x7FF) or any 29-bit extended frame
            if ((rx_msg.identifier >= 0x700 && rx_msg.identifier <= 0x7FF) || rx_msg.extd) {
                if (s_cap_on) {
                    portENTER_CRITICAL(&s_cap_mux);
                    if (s_cap_n < DIAG_CAP_MAX) {
                        diag_cap_t *c = &s_cap[s_cap_n++];
                        c->t_ms = esp_log_timestamp();
                        c->id = rx_msg.identifier;
                        c->extd = rx_msg.extd;
                        c->dlc = rx_msg.data_length_code;
                        memcpy(c->data, rx_msg.data, 8);
                    }
                    portEXIT_CRITICAL(&s_cap_mux);
                }
                ESP_LOGI(TAG, ">>> CAN RX DIAG %s ID 0x%0*" PRIX32 " DLC %d: %02X %02X %02X %02X %02X %02X %02X %02X",
                         rx_msg.extd ? "EXT29" : "STD11", rx_msg.extd ? 8 : 3, rx_msg.identifier, rx_msg.data_length_code,
                         rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3],
                         rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
            }

            if (!(rx_msg.rtr) && !(rx_msg.extd)) {
                guzzi_dbc_parse_frame(rx_msg.identifier, rx_msg.data, rx_msg.data_length_code);
            }
        }
    }
}

esp_err_t guzzi_can_init(void) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_NORMAL);
    // NORMAL mode: our frames must be ACKed by a vehicle node -> TX success now proves a real ECU decoded them.
    // Large RX queue: bus runs ~1000 fps, a 5-deep queue drops diagnostic replies while logging.
    g_config.rx_queue_len = 128;
    g_config.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS;
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

    ESP_LOGI(TAG, "TWAI CAN driver initialized successfully (TX: GPIO %d, RX: GPIO %d, 500kbps, NORMAL mode)", CAN_TX_GPIO, CAN_RX_GPIO);

    xTaskCreate(twai_rx_task, "twai_rx", 4096, NULL, 10, &twai_rx_task_handle);
    return ESP_OK;
}

typedef struct {
    uint32_t id;
    bool is_extd;
    const char *target_name;
    uint32_t resp_id;
} diag_target_t;

static const diag_target_t DASH_TARGETS[] __attribute__((unused)) = {
    { 0x7A0,      false, "COBO TFT Dash 11-bit (0x7A0)" },
    { 0x18DB33F1, true,  "OBD Functional 29-bit (0x18DB33F1)" },
    { 0x18DA60F1, true,  "Dash 29-bit (Target 0x60)" },
    { 0x18DA20F1, true,  "Dash 29-bit (Target 0x20)" },
    { 0x18DA17F1, true,  "Dash 29-bit (Target 0x17)" },
    { 0x18DA28F1, true,  "Dash 29-bit (Target 0x28)" },
    { 0x720,      false, "Dash 11-bit Standard (0x720)" }
};

static const diag_target_t ECU_TARGETS[] = {
    { 0x7DF,      false, "OBD Functional 11-bit (0x7DF)" },
    { 0x18DB33F1, true,  "OBD Functional 29-bit (0x18DB33F1)" },
    { 0x18DA00F1, true,  "Engine ECU 29-bit (Target 0x00)" },
    { 0x18DA10F1, true,  "Engine ECU 29-bit (Target 0x10)" },
    { 0x7E0,      false, "Engine ECU 11-bit Standard (0x7E0)" }
};

typedef enum { TX_ACKED = 0, TX_NO_ACK, TX_QUEUE_ERR } tx_result_t;

static const char *tx_result_str(tx_result_t r) __attribute__((unused));
static const char *tx_result_str(tx_result_t r) {
    switch (r) {
        case TX_ACKED:  return "acked";
        case TX_NO_ACK: return "no_ack";
        default:        return "queue_error";
    }
}

/**
 * Single-shot transmit (no automatic retransmission, so a missing ACK can never flood the bus).
 * Waits for the frame to leave the controller and reports whether a vehicle node ACKed it.
 * Retries up to 3 times on lost arbitration (busy bus).
 */
static tx_result_t send_diag_frame(const diag_target_t *tgt, const uint8_t *data, uint8_t len) {
    twai_message_t tx_msg = {
        .identifier = tgt->id,
        .extd = tgt->is_extd ? 1 : 0,
        .ss = 0,
        .data_length_code = len
    };
    memcpy(tx_msg.data, data, len);

    for (int attempt = 0; attempt < 3; attempt++) {
        twai_status_info_t before, after;
        twai_get_status_info(&before);

        esp_err_t err = twai_transmit(&tx_msg, pdMS_TO_TICKS(50));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TX %s ID 0x%0*" PRIX32 " queue failed: %s", tgt->target_name,
                     tgt->is_extd ? 8 : 3, tgt->id, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(150)); // allow bus-off auto-recovery
            continue;
        }

        // Wait for the frame to complete (a frame is ~0.25 ms; allow up to ~50 ms)
        for (int i = 0; i < 25; i++) {
            vTaskDelay(1);
            twai_get_status_info(&after);
            if (after.msgs_to_tx == 0) break;
        }

        if (after.tx_failed_count == before.tx_failed_count && after.msgs_to_tx == 0) {
            ESP_LOGI(TAG, "TX %s ID 0x%0*" PRIX32 " [%02X %02X %02X %02X %02X %02X %02X %02X] -> ACKED by vehicle",
                     tgt->target_name, tgt->is_extd ? 8 : 3, tgt->id,
                     data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
            return TX_ACKED;
        }
        if (after.arb_lost_count > before.arb_lost_count) {
            continue; // lost arbitration to bike traffic, retry
        }
        ESP_LOGW(TAG, "TX %s ID 0x%0*" PRIX32 " -> NOT ACKED (TEC %" PRIu32 ", BusErr %" PRIu32 ")",
                 tgt->target_name, tgt->is_extd ? 8 : 3, tgt->id, after.tx_error_counter, after.bus_error_count);
        return TX_NO_ACK;
    }
    return TX_QUEUE_ERR;
}

// ============================================================================
// MAGNETI MARELLI UDS SECURITY ACCESS ALGORITHM (WICHMANN-HILL PRNG / SCHRAGE)
// Constants: 0x1759 (Lancia Delta S4 1759cc) and 0x1215
// ============================================================================
static uint16_t marelli_hamB76(int16_t x) {
    int16_t q = x / 178;
    int16_t r = x % 178;
    int32_t res = 170 * r - 63 * q;
    if (res < 0) res += 30323;
    return (uint16_t)(res & 0xFFFF);
}

static uint16_t marelli_hamEA4(int16_t x) {
    int16_t q = x / 177;
    int16_t r = x % 177;
    int32_t res = 171 * r - 2 * q;
    if (res < 0) res += 30269;
    return (uint16_t)(res & 0xFFFF);
}

static uint16_t marelli_hamA6D(uint16_t c, uint16_t s) {
    uint16_t v1 = (marelli_hamB76((int16_t)c) + marelli_hamEA4((int16_t)s)) & 0xFFFF;
    uint16_t v3 = marelli_hamB76((int16_t)s);
    uint16_t v4 = marelli_hamEA4((int16_t)v1);
    return (v3 + v4) & 0xFFFF;
}

void guzzi_marelli_calc_key(const uint8_t seed[4], uint8_t key[4]) {
    uint16_t s_hi = ((uint16_t)seed[0] << 8) | seed[1];
    uint16_t s_lo = ((uint16_t)seed[2] << 8) | seed[3];
    uint16_t k_hi = marelli_hamA6D(0x1759, s_hi);
    uint16_t k_lo = marelli_hamA6D(0x1215, s_lo);
    key[0] = (uint8_t)(k_hi >> 8);
    key[1] = (uint8_t)(k_hi & 0xFF);
    key[2] = (uint8_t)(k_lo >> 8);
    key[3] = (uint8_t)(k_lo & 0xFF);
}

esp_err_t guzzi_can_reset_service_wrench(char *out_json, size_t out_len) {
    ESP_LOGI(TAG, "=== SERVICE WRENCH RESET: Targeting COBO TFT Dash (0x7E3) + Marelli 11MP (0x7A0) ===");
    log_twai_status("ServiceReset: Start");

    portENTER_CRITICAL(&s_cap_mux);
    s_cap_n = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_cap_on = true;

    diag_target_t tgt_dash = { 0x7E3, false, "COBO TFT Dash (0x7E3)", 0x7EB };
    diag_target_t tgt_ecu  = { 0x7A0, false, "Marelli 11MP ECU (0x7A0)", 0x7A8 };

    // --- PHASE 1: COBO TFT DASHBOARD (0x7E3) ---
    // 1. Ensure Default Diagnostic Session
    uint8_t d_def[8] = {0x02, 0x10, 0x01, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt_dash, d_def, 8);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. KWP Clear DTCs (Group FF 00 - confirmed positive response 0x54)
    uint8_t d_clr[8] = {0x03, 0x14, 0xFF, 0x00, 0, 0, 0, 0};
    send_diag_frame(&tgt_dash, d_clr, 8);
    vTaskDelay(pdMS_TO_TICKS(80));

    // 3. Write LocalID 0x40 = 0x00 (Clear Maintenance Reminder Flag)
    uint8_t d_w40[8] = {0x03, 0x3B, 0x40, 0x00, 0, 0, 0, 0};
    send_diag_frame(&tgt_dash, d_w40, 8);
    vTaskDelay(pdMS_TO_TICKS(80));

    // 4. InputOutputControl LocalID 0x40 = 0x00
    uint8_t d_io40[8] = {0x03, 0x30, 0x40, 0x00, 0, 0, 0, 0};
    send_diag_frame(&tgt_dash, d_io40, 8);
    vTaskDelay(pdMS_TO_TICKS(80));

    // 5. Read back LocalID 0x40 to check status
    uint8_t d_r40[8] = {0x02, 0x21, 0x40, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt_dash, d_r40, 8);
    vTaskDelay(pdMS_TO_TICKS(80));

    // --- PHASE 2: MARELLI 11MP ENGINE ECU (0x7A0) ---
    uint8_t e_sess[8] = {0x02, 0x10, 0x03, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt_ecu, e_sess, 8);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t e_clr[8] = {0x04, 0x14, 0xFF, 0xFF, 0xFF, 0, 0, 0};
    send_diag_frame(&tgt_ecu, e_clr, 8);
    vTaskDelay(pdMS_TO_TICKS(80));

    // Restore Default Session on ECU
    uint8_t e_def[8] = {0x02, 0x10, 0x01, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt_ecu, e_def, 8);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_cap_on = false;

    int n_replies = s_cap_n;
    ESP_LOGI(TAG, "=== SERVICE RESET COMPLETE: %d diagnostic replies captured ===", n_replies);

    if (out_json && out_len > 0) {
        size_t o = 0;
#define APPEND(...) do { if (o < out_len) o += snprintf(out_json + o, out_len - o, __VA_ARGS__); } while (0)
        APPEND("{\"target_dash\":\"0x7E3\",\"target_ecu\":\"0x7A0\",\"replies\":[");
        for (int i = 0; i < n_replies; i++) {
            const diag_cap_t *c = &s_cap[i];
            APPEND("%s{\"id\":\"0x%03" PRIX32 "\",\"t\":%" PRIu32 ",\"data\":\"%02X %02X %02X %02X %02X %02X %02X %02X\"}",
                   i ? "," : "", c->id, c->t_ms,
                   c->data[0], c->data[1], c->data[2], c->data[3], c->data[4], c->data[5], c->data[6], c->data[7]);
        }
        APPEND("]}");
#undef APPEND
    }

    log_twai_status("ServiceReset: Done");
    return ESP_OK;
}

esp_err_t guzzi_can_clear_ecu_dtc(void) {
    ESP_LOGI(TAG, "Executing UDS Clear DTC on Engine ECU targets...");
    log_twai_status("ClearDTC: Start");

    uint8_t obd_ping[8]   = {0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // OBD-II Mode 01 PID 00 ping
    uint8_t sess_data[8]  = {0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}; // Extended Session
    uint8_t clear_data[8] = {0x04, 0x14, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00}; // Clear all DTCs

    esp_err_t last_res = ESP_FAIL;

    for (size_t i = 0; i < sizeof(ECU_TARGETS) / sizeof(ECU_TARGETS[0]); i++) {
        const diag_target_t *tgt = &ECU_TARGETS[i];

        // 1. Universal OBD-II Mode 01 PID 00 ping
        if (send_diag_frame(tgt, obd_ping, 8) == TX_ACKED) {
            last_res = ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        // 2. Extended Session
        send_diag_frame(tgt, sess_data, 8);
        vTaskDelay(pdMS_TO_TICKS(100));

        // 3. Clear DTC
        send_diag_frame(tgt, clear_data, 8);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    log_twai_status("ClearDTC: Done");
    return last_res;
}


typedef struct {
    diag_target_t tgt;
    uint8_t data[8];
    const char *what;
} ping_probe_t;

static const ping_probe_t PING_PROBES[] = {
    { { 0x7DF,      false, "OBD Functional 11-bit" }, {0x02, 0x01, 0x00, 0, 0, 0, 0, 0}, "OBD 01 PID00" },
    { { 0x7DF,      false, "OBD Functional 11-bit" }, {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x7A0,      false, "COBO Dash 11-bit 0x7A0" }, {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x7E0,      false, "Engine 11-bit 0x7E0" },   {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x720,      false, "Dash 11-bit 0x720" },     {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x18DB33F1, true,  "OBD Functional 29-bit" }, {0x02, 0x01, 0x00, 0, 0, 0, 0, 0}, "OBD 01 PID00" },
    { { 0x18DB33F1, true,  "OBD Functional 29-bit" }, {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x18DA00F1, true,  "Engine 29-bit 0x00" },    {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x18DA10F1, true,  "Engine 29-bit 0x10" },    {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x18DA60F1, true,  "Dash 29-bit 0x60" },      {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
    { { 0x18DA17F1, true,  "Dash 29-bit 0x17" },      {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0}, "TesterPresent" },
};

esp_err_t guzzi_can_ping(char *out, size_t out_len) {
    ESP_LOGI(TAG, "=== CAN PING: probing OBD/UDS targets (read-only requests) ===");
    log_twai_status("Ping: Start");

    portENTER_CRITICAL(&s_cap_mux);
    s_cap_n = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_cap_on = true;

    const size_t n_probes = sizeof(PING_PROBES) / sizeof(PING_PROBES[0]);
    tx_result_t results[sizeof(PING_PROBES) / sizeof(PING_PROBES[0])];
    uint32_t t_sent[sizeof(PING_PROBES) / sizeof(PING_PROBES[0])];
    int acked = 0;

    for (size_t i = 0; i < n_probes; i++) {
        t_sent[i] = esp_log_timestamp();
        results[i] = send_diag_frame(&PING_PROBES[i].tgt, PING_PROBES[i].data, 8);
        if (results[i] == TX_ACKED) acked++;
        vTaskDelay(pdMS_TO_TICKS(200)); // ECU P2 timeout is 50 ms; 200 ms is plenty
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    s_cap_on = false;

    twai_status_info_t st;
    twai_get_status_info(&st);
    log_twai_status("Ping: Done");

    int n_cap = s_cap_n;
    ESP_LOGI(TAG, "=== CAN PING RESULT: %d/%d frames ACKed, %d diagnostic responses captured ===",
             acked, (int)n_probes, n_cap);

    size_t o = 0;
#define APPEND(...) do { if (o < out_len) o += snprintf(out + o, out_len - o, __VA_ARGS__); } while (0)
    APPEND("{\"acked\":%d,\"probes_total\":%d,\"responses\":%d,", acked, (int)n_probes, n_cap);
    APPEND("\"tec\":%" PRIu32 ",\"rec\":%" PRIu32 ",\"bus_errors\":%" PRIu32 ",\"rx_missed\":%" PRIu32 ",\"rx_overrun\":%" PRIu32 ",",
           st.tx_error_counter, st.rx_error_counter, st.bus_error_count, st.rx_missed_count, st.rx_overrun_count);
    APPEND("\"probes\":[");
    for (size_t i = 0; i < n_probes; i++) {
        APPEND("%s{\"id\":\"0x%0*" PRIX32 "\",\"target\":\"%s\",\"req\":\"%s\",\"tx\":\"%s\",\"t\":%" PRIu32 "}",
               i ? "," : "", PING_PROBES[i].tgt.is_extd ? 8 : 3, PING_PROBES[i].tgt.id,
               PING_PROBES[i].tgt.target_name, PING_PROBES[i].what, tx_result_str(results[i]), t_sent[i]);
    }
    APPEND("],\"rx\":[");
    for (int i = 0; i < n_cap; i++) {
        const diag_cap_t *c = &s_cap[i];
        APPEND("%s{\"id\":\"0x%0*" PRIX32 "\",\"t\":%" PRIu32 ",\"data\":\"%02X %02X %02X %02X %02X %02X %02X %02X\"}",
               i ? "," : "", c->extd ? 8 : 3, c->id, c->t_ms,
               c->data[0], c->data[1], c->data[2], c->data[3], c->data[4], c->data[5], c->data[6], c->data[7]);
    }
    APPEND("]}");
#undef APPEND
    return ESP_OK;
}

typedef struct {
    uint32_t req_id;
    bool extd;
    uint32_t resp_id;
    uint8_t resp_len;
    uint8_t resp_data[8];
} discovered_node_t;

esp_err_t guzzi_can_scan_bus(char *out, size_t out_len) {
    ESP_LOGI(TAG, "=== CAN DIAGNOSTIC SWEEP: Scanning 0x700..0x7EF + 29-bit UDS ===");
    log_twai_status("ScanBus: Start");

    discovered_node_t found[16];
    int n_found = 0;

    portENTER_CRITICAL(&s_cap_mux);
    s_cap_n = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_cap_on = true;

    // 1. Scan 11-bit standard diagnostic IDs: 0x700 to 0x7EF
    for (uint32_t id = 0x700; id <= 0x7EF; id++) {
        diag_target_t tgt = { id, false, "Scan Target" };
        uint8_t req[8] = {0x02, 0x10, 0x03, 0, 0, 0, 0, 0}; // ExtSession

        portENTER_CRITICAL(&s_cap_mux);
        s_cap_n = 0;
        portEXIT_CRITICAL(&s_cap_mux);

        send_diag_frame(&tgt, req, 8);
        vTaskDelay(pdMS_TO_TICKS(10));

        // Fallback: TesterPresent if no reply
        if (s_cap_n == 0) {
            uint8_t tp[8] = {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0};
            send_diag_frame(&tgt, tp, 8);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_cap_n > 0) {
            diag_cap_t tmp_cap[8];
            int n_tmp = 0;
            portENTER_CRITICAL(&s_cap_mux);
            n_tmp = s_cap_n > 8 ? 8 : s_cap_n;
            for (int k = 0; k < n_tmp; k++) {
                tmp_cap[k] = s_cap[k];
            }
            portEXIT_CRITICAL(&s_cap_mux);

            for (int k = 0; k < n_tmp; k++) {
                uint32_t r_id = tmp_cap[k].id;
                if (r_id == id) continue; // Ignore echo
                bool already = false;
                for (int f = 0; f < n_found; f++) {
                    if (found[f].req_id == id && found[f].resp_id == r_id) {
                        already = true;
                        break;
                    }
                }
                if (!already && n_found < 16) {
                    found[n_found].req_id = id;
                    found[n_found].extd = false;
                    found[n_found].resp_id = r_id;
                    found[n_found].resp_len = tmp_cap[k].dlc;
                    memcpy(found[n_found].resp_data, tmp_cap[k].data, 8);
                    ESP_LOGI(TAG, "!!! DISCOVERED 11-BIT NODE: Req 0x%03" PRIX32 " -> Resp 0x%03" PRIX32 " [%02X %02X %02X %02X] !!!",
                             id, r_id, tmp_cap[k].data[0], tmp_cap[k].data[1], tmp_cap[k].data[2], tmp_cap[k].data[3]);
                    n_found++;
                }
            }
        }
    }

    // 2. Scan standard 29-bit UDS targets
    static const uint32_t EXT29_TARGETS[] = {
        0x18DA60F1, 0x18DA17F1, 0x18DA28F1, 0x18DA20F1,
        0x18DA40F1, 0x18DA58F1, 0x18DA00F1, 0x18DA10F1,
        0x18DAF160, 0x18DAF117,
        0x18DB33F1
    };
    for (size_t i = 0; i < sizeof(EXT29_TARGETS)/sizeof(EXT29_TARGETS[0]); i++) {
        uint32_t id = EXT29_TARGETS[i];
        diag_target_t tgt = { id, true, "Scan Target 29b" };
        uint8_t req[8] = {0x02, 0x10, 0x03, 0, 0, 0, 0, 0};

        portENTER_CRITICAL(&s_cap_mux);
        s_cap_n = 0;
        portEXIT_CRITICAL(&s_cap_mux);

        send_diag_frame(&tgt, req, 8);
        vTaskDelay(pdMS_TO_TICKS(15));

        if (s_cap_n == 0) {
            uint8_t tp[8] = {0x02, 0x3E, 0x00, 0, 0, 0, 0, 0};
            send_diag_frame(&tgt, tp, 8);
            vTaskDelay(pdMS_TO_TICKS(15));
        }

        if (s_cap_n > 0) {
            diag_cap_t tmp_cap[8];
            int n_tmp = 0;
            portENTER_CRITICAL(&s_cap_mux);
            n_tmp = s_cap_n > 8 ? 8 : s_cap_n;
            for (int k = 0; k < n_tmp; k++) {
                tmp_cap[k] = s_cap[k];
            }
            portEXIT_CRITICAL(&s_cap_mux);

            for (int k = 0; k < n_tmp; k++) {
                uint32_t r_id = tmp_cap[k].id;
                if (r_id == id) continue; // Ignore echo
                bool already = false;
                for (int f = 0; f < n_found; f++) {
                    if (found[f].req_id == id && found[f].resp_id == r_id) {
                        already = true;
                        break;
                    }
                }
                if (!already && n_found < 16) {
                    found[n_found].req_id = id;
                    found[n_found].extd = true;
                    found[n_found].resp_id = r_id;
                    found[n_found].resp_len = tmp_cap[k].dlc;
                    memcpy(found[n_found].resp_data, tmp_cap[k].data, 8);
                    ESP_LOGI(TAG, "!!! DISCOVERED 29-BIT NODE: Req 0x%08" PRIX32 " -> Resp 0x%08" PRIX32 " [%02X %02X %02X %02X] !!!",
                             id, r_id, tmp_cap[k].data[0], tmp_cap[k].data[1], tmp_cap[k].data[2], tmp_cap[k].data[3]);
                    n_found++;
                }
            }
        }
    }

    s_cap_on = false;
    ESP_LOGI(TAG, "=== CAN DIAGNOSTIC SWEEP COMPLETE: %d active nodes discovered ===", n_found);

    if (out && out_len > 0) {
        size_t o = 0;
#define APPEND(...) do { if (o < out_len) o += snprintf(out + o, out_len - o, __VA_ARGS__); } while (0)
        APPEND("{\"nodes_found\":%d,\"nodes\":[", n_found);
        for (int i = 0; i < n_found; i++) {
            APPEND("%s{\"req_id\":\"0x%0*" PRIX32 "\",\"resp_id\":\"0x%0*" PRIX32 "\",\"data\":\"",
                   i ? "," : "",
                   found[i].extd ? 8 : 3, found[i].req_id,
                   found[i].extd ? 8 : 3, found[i].resp_id);
            for (int d = 0; d < found[i].resp_len; d++) {
                APPEND("%s%02X", d ? " " : "", found[i].resp_data[d]);
            }
            APPEND("\"}");
        }
        APPEND("]}");
#undef APPEND
    }
    return ESP_OK;
}


esp_err_t guzzi_can_dominant_test(uint32_t hold_ms) {
    if (hold_ms == 0) hold_ms = 60000;
    uint32_t idle_ms = esp_log_timestamp() - s_last_rx_ms;
    if (s_last_rx_ms != 0 && idle_ms < 1500) {
        ESP_LOGE(TAG, "Dominant test REFUSED: bus is active (traffic detected %" PRIu32 " ms ago). Turn ignition OFF first.", idle_ms);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "======================================================================");
    ESP_LOGW(TAG, "=== 60-SEC DOMINANT TEST: forcing CTX (GPIO 5 & 21) LOW for %" PRIu32 " ms ===", hold_ms);
    ESP_LOGW(TAG, "======================================================================");

    s_twai_paused = true;
    vTaskDelay(pdMS_TO_TICKS(150));
    twai_stop();

    // Drive both GPIO 5 (D3) and GPIO 21 (silkscreen TX) LOW
    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(CAN_TX_GPIO, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(CAN_TX_GPIO, 0);

    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(GPIO_NUM_21, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(GPIO_NUM_21, 0);

    for (uint32_t elapsed = 0; elapsed < hold_ms; elapsed += 5000) {
        uint32_t remain = (hold_ms - elapsed) / 1000;
        ESP_LOGI(TAG, "[TEST LOW - %" PRIu32 "s remaining] CTX=0V. CHECK: Pin1(TXD)=0V, Pin8(Rs)=0V!, Pin3(VCC)=3.3V, CANH-CANL=1.5-2V", remain);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Restore recessive (HIGH) and restart TWAI
    gpio_set_level(CAN_TX_GPIO, 1);
    esp_rom_gpio_connect_out_signal(CAN_TX_GPIO, TWAI_TX_IDX, false, false);
    gpio_set_level(GPIO_NUM_21, 1);
    esp_rom_gpio_connect_out_signal(GPIO_NUM_21, TWAI_TX_IDX, false, false);

    esp_err_t err = twai_start();
    s_twai_paused = false;

    ESP_LOGW(TAG, "=== TEST COMPLETE: CTX released to recessive (3.3V). TWAI restart: %s ===", esp_err_to_name(err));
    return err;
}

esp_err_t guzzi_can_toggle_test(uint32_t total_ms) {
    if (total_ms == 0) total_ms = 60000;
    uint32_t idle_ms = esp_log_timestamp() - s_last_rx_ms;
    if (s_last_rx_ms != 0 && idle_ms < 1500) {
        ESP_LOGE(TAG, "Toggle test REFUSED: bus is active. Turn ignition OFF first.");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "=== 60-SEC TOGGLE TEST: Pulsing CTX LOW 1s / HIGH 1s for %" PRIu32 " ms ===", total_ms);
    s_twai_paused = true;
    vTaskDelay(pdMS_TO_TICKS(150));
    twai_stop();

    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(CAN_TX_GPIO, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(GPIO_NUM_21, SIG_GPIO_OUT_IDX, false, false);

    for (uint32_t t = 0; t < total_ms; t += 2000) {
        gpio_set_level(CAN_TX_GPIO, 0);
        gpio_set_level(GPIO_NUM_21, 0);
        ESP_LOGI(TAG, "[TOGGLE %" PRIu32 "s] CTX -> LOW  (0V) [Dominant: CANH-CANL should jump to ~2V]", t / 1000);
        vTaskDelay(pdMS_TO_TICKS(1000));

        gpio_set_level(CAN_TX_GPIO, 1);
        gpio_set_level(GPIO_NUM_21, 1);
        ESP_LOGI(TAG, "[TOGGLE %" PRIu32 "s] CTX -> HIGH (3.3V) [Recessive: CANH-CANL should drop to 0V]", (t + 1000) / 1000);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_rom_gpio_connect_out_signal(CAN_TX_GPIO, TWAI_TX_IDX, false, false);
    esp_rom_gpio_connect_out_signal(GPIO_NUM_21, TWAI_TX_IDX, false, false);
    esp_err_t err = twai_start();
    s_twai_paused = false;

    ESP_LOGW(TAG, "=== TOGGLE TEST COMPLETE. TWAI restart: %s ===", esp_err_to_name(err));
    return err;
}

typedef struct {
    uint8_t data[8];
    uint8_t len;
    const char *name;
} dash_probe_item_t;

static const dash_probe_item_t DASH_SCAN_ITEMS[] = {
    // 1. KWP2000 ReadECUIdentification (Service 0x1A)
    { {0x02, 0x1A, 0x90, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x90 (VIN)" },
    { {0x02, 0x1A, 0x91, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x91 (Hardware/SW ID)" },
    { {0x02, 0x1A, 0x92, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x92" },
    { {0x02, 0x1A, 0x93, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x93 (Service Record)" },
    { {0x02, 0x1A, 0x9A, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x9A" },
    { {0x02, 0x1A, 0x80, 0, 0, 0, 0, 0}, 8, "ReadECUID 0x80" },

    // 2. KWP2000 RoutineControl (Service 0x31) - Panigale V2 COBO Cluster (0x09)
    { {0x02, 0x31, 0x09, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x09 (Panigale 2B)" },
    { {0x03, 0x31, 0x09, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x09 0x00 (Panigale 3B)" },
    { {0x04, 0x31, 0x09, 0x00, 0x00, 0, 0, 0}, 8, "StartRoutine 0x09 0x00 0x00 (Panigale 4B)" },
    { {0x02, 0x33, 0x09, 0, 0, 0, 0, 0}, 8, "RequestResults 0x09" },
    { {0x03, 0x32, 0x09, 0x00, 0, 0, 0, 0}, 8, "StopRoutine 0x09 0x00" },

    // 3. KWP2000 RoutineControl - Monster / Scrambler COBO Cluster (0x07)
    { {0x02, 0x31, 0x07, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x07 (Scrambler 2B)" },
    { {0x03, 0x31, 0x07, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x07 0x00 (Scrambler 3B)" },
    { {0x04, 0x31, 0x07, 0x00, 0x00, 0, 0, 0}, 8, "StartRoutine 0x07 0x00 0x00 (Scrambler 4B)" },
    { {0x02, 0x33, 0x07, 0, 0, 0, 0, 0}, 8, "RequestResults 0x07" },
    { {0x03, 0x32, 0x07, 0x00, 0, 0, 0, 0}, 8, "StopRoutine 0x07 0x00" },

    // 4. KWP2000 RoutineControl - LocalID 0x40 Match (Service flag is at LocalID 0x40)
    { {0x02, 0x31, 0x40, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x40 (2B)" },
    { {0x03, 0x31, 0x40, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x40 0x00 (3B)" },
    { {0x04, 0x31, 0x40, 0x00, 0x00, 0, 0, 0}, 8, "StartRoutine 0x40 0x00 0x00 (4B)" },
    { {0x02, 0x33, 0x40, 0, 0, 0, 0, 0}, 8, "RequestResults 0x40" },
    { {0x03, 0x32, 0x40, 0x00, 0, 0, 0, 0}, 8, "StopRoutine 0x40 0x00" },

    // 5. OBDSTAR COBO TFT 5" Service Password "123" variants
    { {0x04, 0x31, 0x09, 0x01, 0x23, 0, 0, 0}, 8, "StartRoutine 0x09 [01 23]" },
    { {0x04, 0x31, 0x09, 0x00, 0x7B, 0, 0, 0}, 8, "StartRoutine 0x09 [00 7B]" },
    { {0x04, 0x31, 0x40, 0x01, 0x23, 0, 0, 0}, 8, "StartRoutine 0x40 [01 23]" },
    { {0x04, 0x31, 0x40, 0x00, 0x7B, 0, 0, 0}, 8, "StartRoutine 0x40 [00 7B]" },
    { {0x04, 0x31, 0x01, 0x01, 0x23, 0, 0, 0}, 8, "StartRoutine 0x01 [01 23]" },
    { {0x04, 0x31, 0x01, 0x00, 0x7B, 0, 0, 0}, 8, "StartRoutine 0x01 [00 7B]" },
    { {0x04, 0x31, 0x07, 0x01, 0x23, 0, 0, 0}, 8, "StartRoutine 0x07 [01 23]" },
    { {0x04, 0x31, 0x07, 0x00, 0x7B, 0, 0, 0}, 8, "StartRoutine 0x07 [00 7B]" },

    // 6. Generic Italian Routine IDs (0x01, 0x02, 0x08, 0xFF)
    { {0x02, 0x31, 0x01, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x01" },
    { {0x03, 0x31, 0x01, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x01 0x00" },
    { {0x02, 0x31, 0x02, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x02" },
    { {0x03, 0x31, 0x02, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x02 0x00" },
    { {0x02, 0x31, 0x08, 0, 0, 0, 0, 0}, 8, "StartRoutine 0x08" },
    { {0x03, 0x31, 0x08, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0x08 0x00" },
    { {0x02, 0x31, 0xFF, 0, 0, 0, 0, 0}, 8, "StartRoutine 0xFF" },
    { {0x03, 0x31, 0xFF, 0x00, 0, 0, 0, 0}, 8, "StartRoutine 0xFF 0x00" },

    // 7. SecurityAccess Sub-levels (check if alternative levels are supported)
    { {0x02, 0x27, 0x03, 0, 0, 0, 0, 0}, 8, "SecAccess ReqSeed L3 (0x27 0x03)" },
    { {0x02, 0x27, 0x05, 0, 0, 0, 0, 0}, 8, "SecAccess ReqSeed L5 (0x27 0x05)" },
    { {0x02, 0x27, 0x61, 0, 0, 0, 0, 0}, 8, "SecAccess ReqSeed L61 (0x27 0x61)" },

    // 8. Service Status Readback
    { {0x02, 0x21, 0x40, 0, 0, 0, 0, 0}, 8, "Read LocalID 0x40 (Service Flag)" },
    { {0x02, 0x21, 0x41, 0, 0, 0, 0, 0}, 8, "Read LocalID 0x41 (Service Counter)" },
    { {0x02, 0x21, 0x43, 0, 0, 0, 0, 0}, 8, "Read LocalID 0x43 (Service Log)" }
};

/**
 * ISO 15765-2 (ISO-TP) multi-frame transceiver for UDS queries over CAN.
 * Handles Single Frame (SF) and First Frame (FF) + Flow Control (FC) + Consecutive Frames (CF).
 */
static void transceive_uds_isotp(
    const diag_target_t *tgt,
    const uint8_t *req,
    uint8_t req_len,
    char *out_hex, size_t out_hex_sz,
    char *out_ascii, size_t out_ascii_sz,
    char *out_status, size_t out_status_sz)
{
    snprintf(out_hex, out_hex_sz, "NO_REPLY");
    if (out_ascii_sz > 0) out_ascii[0] = '\0';
    snprintf(out_status, out_status_sz, "NO_REPLY");

    portENTER_CRITICAL(&s_cap_mux);
    s_cap_n = 0;
    portEXIT_CRITICAL(&s_cap_mux);
    s_cap_on = true;

    // Send the diagnostic request frame
    send_diag_frame(tgt, req, req_len);

    uint32_t exp_id = tgt->resp_id ? tgt->resp_id : (tgt->id + 8);

    // Wait up to 150 ms for initial response frame (Single Frame 0x0x or First Frame 0x1x)
    uint32_t start_ms = esp_log_timestamp();
    int first_idx = -1;
    while ((esp_log_timestamp() - start_ms) < 150) {
        vTaskDelay(pdMS_TO_TICKS(10));
        portENTER_CRITICAL(&s_cap_mux);
        for (int i = 0; i < s_cap_n; i++) {
            if (s_cap[i].id == exp_id) {
                first_idx = i;
                break;
            }
        }
        portEXIT_CRITICAL(&s_cap_mux);
        if (first_idx >= 0) break;
    }

    if (first_idx < 0) {
        s_cap_on = false;
        return;
    }

    diag_cap_t f0;
    portENTER_CRITICAL(&s_cap_mux);
    f0 = s_cap[first_idx];
    portEXIT_CRITICAL(&s_cap_mux);

    uint8_t pci = f0.data[0] & 0xF0;
    uint8_t full_payload[256];
    size_t payload_len = 0;

    if (pci == 0x00) {
        // Single Frame: data[0] is length (1..7)
        uint8_t sf_len = f0.data[0] & 0x0F;
        if (sf_len > 7) sf_len = 7;
        memcpy(full_payload, &f0.data[1], sf_len);
        payload_len = sf_len;
    } else if (pci == 0x10) {
        // First Frame of Multi-Frame transmission
        size_t total_len = ((f0.data[0] & 0x0F) << 8) | f0.data[1];
        if (total_len > sizeof(full_payload)) total_len = sizeof(full_payload);

        // Copy initial 6 payload bytes
        memcpy(full_payload, &f0.data[2], 6);
        payload_len = 6;

        // Clear capture buffer to receive Consecutive Frames cleanly
        portENTER_CRITICAL(&s_cap_mux);
        s_cap_n = 0;
        portEXIT_CRITICAL(&s_cap_mux);

        // Send ISO-TP Flow Control frame (CTS: 0x30, BlockSize: 0, STmin: 0 ms)
        uint8_t fc[8] = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        send_diag_frame(tgt, fc, 8);

        // Wait for Consecutive Frames (CF: 0x21, 0x22, 0x23, ...)
        uint32_t cf_start = esp_log_timestamp();
        int expected_sn = 1;
        while ((esp_log_timestamp() - cf_start) < 250 && payload_len < total_len) {
            vTaskDelay(pdMS_TO_TICKS(10));
            portENTER_CRITICAL(&s_cap_mux);
            for (int i = 0; i < s_cap_n; i++) {
                if (s_cap[i].id == exp_id && (s_cap[i].data[0] & 0xF0) == 0x20) {
                    uint8_t sn = s_cap[i].data[0] & 0x0F;
                    if (sn == (expected_sn & 0x0F)) {
                        size_t remain = total_len - payload_len;
                        size_t chunk = (remain > 7) ? 7 : remain;
                        memcpy(full_payload + payload_len, &s_cap[i].data[1], chunk);
                        payload_len += chunk;
                        expected_sn++;
                    }
                }
            }
            portEXIT_CRITICAL(&s_cap_mux);
        }
    } else {
        // Raw frame fallback
        memcpy(full_payload, f0.data, 8);
        payload_len = 8;
    }

    s_cap_on = false;

    // Format assembled payload as hex string
    size_t ho = 0;
    for (size_t i = 0; i < payload_len && ho + 3 < out_hex_sz; i++) {
        ho += snprintf(out_hex + ho, out_hex_sz - ho, "%s%02X", i ? " " : "", full_payload[i]);
    }

    // Determine status & extract printable ASCII text
    if (payload_len >= 1) {
        uint8_t sid = full_payload[0];
        if (sid == 0x7F && payload_len >= 3) {
            uint8_t rej_sid = full_payload[1];
            uint8_t nrc = full_payload[2];
            const char *nrc_name = "ERROR";
            if (nrc == 0x11) nrc_name = "serviceNotSupported";
            else if (nrc == 0x12) nrc_name = "subFunctionNotSupported";
            else if (nrc == 0x13) nrc_name = "incorrectMsgLength";
            else if (nrc == 0x22) nrc_name = "conditionsNotCorrect";
            else if (nrc == 0x24) nrc_name = "requestSequenceError";
            else if (nrc == 0x31) nrc_name = "requestOutOfRange";
            else if (nrc == 0x33) nrc_name = "securityAccessDenied";
            else if (nrc == 0x35) nrc_name = "invalidKey";
            else if (nrc == 0x36) nrc_name = "exceedNumOfAttempts";
            else if (nrc == 0x37) nrc_name = "requiredTimeDelayNotExpired";
            snprintf(out_status, out_status_sz, "NRC 0x%02X (%s, rej 0x%02X)", nrc, nrc_name, rej_sid);
        } else if (sid == (req[1] + 0x40)) {
            snprintf(out_status, out_status_sz, "POSITIVE (0x%02X, %zuB)", sid, payload_len);

            // For ReadDataByIdentifier (0x62), data bytes begin after [62, DID_hi, DID_lo]
            size_t data_start = (sid == 0x62 && payload_len >= 3) ? 3 : 1;
            size_t ao = 0;
            for (size_t i = data_start; i < payload_len && ao + 1 < out_ascii_sz; i++) {
                uint8_t b = full_payload[i];
                if (b >= 32 && b <= 126) {
                    out_ascii[ao++] = (char)b;
                } else if (b == 0) {
                    // skip null terminators
                } else {
                    out_ascii[ao++] = '.';
                }
            }
            out_ascii[ao] = '\0';
        } else {
            snprintf(out_status, out_status_sz, "RESP 0x%02X (%zuB)", sid, payload_len);
        }
    }
}

esp_err_t guzzi_can_probe_dash(char *out, size_t out_len) {
    ESP_LOGI(TAG, "=== COBO DASH PROBE: Targeting ONLY 0x7E3 (Resp 0x7EB) ===");
    log_twai_status("ProbeDash: Start");

    static const diag_target_t tgt = { 0x7E3, false, "COBO TFT Dash (0x7E3)", 0x7EB };
    const size_t n_items = sizeof(DASH_SCAN_ITEMS) / sizeof(DASH_SCAN_ITEMS[0]);

    // 1. Initial wake-up: Ensure Default Diagnostic Session
    uint8_t def_sess[8] = {0x02, 0x10, 0x01, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt, def_sess, 8);
    vTaskDelay(pdMS_TO_TICKS(50));

    size_t o = 0;
#define APPEND(...) do { if (o < out_len) o += snprintf(out + o, out_len - o, __VA_ARGS__); } while (0)
    APPEND("{\"target\":\"0x7E3\",\"response_id\":\"0x7EB\",\"results\":[");

    for (size_t i = 0; i < n_items; i++) {
        const dash_probe_item_t *item = &DASH_SCAN_ITEMS[i];

        char hex_str[256];
        char ascii_str[128];
        char status_str[96];

        transceive_uds_isotp(&tgt, item->data, item->len,
                             hex_str, sizeof(hex_str),
                             ascii_str, sizeof(ascii_str),
                             status_str, sizeof(status_str));

        ESP_LOGI(TAG, "PROBE 0x7E3 [%s] -> %s | %s%s%s%s",
                 item->name, hex_str, status_str,
                 ascii_str[0] ? " | \"" : "",
                 ascii_str[0] ? ascii_str : "",
                 ascii_str[0] ? "\"" : "");

        APPEND("%s{\"name\":\"%s\",\"req\":\"%02X %02X %02X %02X %02X %02X %02X %02X\",\"hex\":\"%s\",\"status\":\"%s\",\"ascii\":\"%s\"}",
               i ? "," : "", item->name,
               item->data[0], item->data[1], item->data[2], item->data[3],
               item->data[4], item->data[5], item->data[6], item->data[7],
               hex_str, status_str, ascii_str);

        vTaskDelay(pdMS_TO_TICKS(80));

        // Periodically refresh DefaultSession to keep cluster session state clean
        if (i > 0 && (i % 6) == 0) {
            send_diag_frame(&tgt, def_sess, 8);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }

    APPEND("]}");
#undef APPEND

    log_twai_status("ProbeDash: Done");
    return ESP_OK;
}

esp_err_t guzzi_can_test_security_key(int cand_idx, const char *custom_key_hex, char *out, size_t out_len) {
    ESP_LOGI(TAG, "=== COBO DASH: Testing SecurityAccess Key Candidate %d ===", cand_idx);
    log_twai_status("SecTest: Start");

    static const diag_target_t tgt = { 0x7E3, false, "COBO TFT Dash (0x7E3)", 0x7EB };

    // 1. Wake up cluster / ensure Default Diagnostic Session
    uint8_t def_sess[8] = {0x02, 0x10, 0x01, 0, 0, 0, 0, 0};
    send_diag_frame(&tgt, def_sess, 8);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Request Seed (Level 1)
    uint8_t req_seed[8] = { 0x02, 0x27, 0x01, 0, 0, 0, 0, 0 };
    char s_hex[64], s_asc[32], s_stat[64];
    transceive_uds_isotp(&tgt, req_seed, 8, s_hex, sizeof(s_hex), s_asc, sizeof(s_asc), s_stat, sizeof(s_stat));

    uint8_t seed[4] = {0};
    bool got_seed = false;
    portENTER_CRITICAL(&s_cap_mux);
    for (int k = 0; k < s_cap_n; k++) {
        if (s_cap[k].id == tgt.resp_id && s_cap[k].data[1] == 0x67 && s_cap[k].data[2] == 0x01) {
            seed[0] = s_cap[k].data[3];
            seed[1] = s_cap[k].data[4];
            seed[2] = s_cap[k].data[5];
            seed[3] = s_cap[k].data[6];
            got_seed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_cap_mux);

    if (!got_seed) {
        ESP_LOGE(TAG, "SecTest: Failed to obtain seed from 0x7E3 (%s)", s_stat);
        if (out && out_len > 0) {
            snprintf(out, out_len, "{\"status\":\"NO_SEED\",\"message\":\"%s\"}", s_stat);
        }
        return ESP_FAIL;
    }

    const char *cand_name = "Unknown";
    uint8_t key[4] = {0};

    switch (cand_idx) {
        case 0:
            cand_name = "Echo (key = seed)";
            key[0] = seed[0]; key[1] = seed[1]; key[2] = seed[2]; key[3] = seed[3];
            break;
        case 1:
            cand_name = "OBDSTAR BCD 123 (00 00 01 23)";
            key[0] = 0x00; key[1] = 0x00; key[2] = 0x01; key[3] = 0x23;
            break;
        case 2:
            cand_name = "OBDSTAR Hex 123 (00 00 00 7B)";
            key[0] = 0x00; key[1] = 0x00; key[2] = 0x00; key[3] = 0x7B;
            break;
        case 3:
            cand_name = "ASCII 123 (31 32 33 00)";
            key[0] = 0x31; key[1] = 0x32; key[2] = 0x33; key[3] = 0x00;
            break;
        case 4:
            cand_name = "ASCII 0123 (30 31 32 33)";
            key[0] = 0x30; key[1] = 0x31; key[2] = 0x32; key[3] = 0x33;
            break;
        case 5:
            cand_name = "XOR COBO (seed ^ 'COBO')";
            key[0] = seed[0] ^ 0x43; key[1] = seed[1] ^ 0x4F; key[2] = seed[2] ^ 0x42; key[3] = seed[3] ^ 0x4F;
            break;
        case 6:
            cand_name = "XOR 0xA5 (seed ^ A5A5A5A5)";
            key[0] = seed[0] ^ 0xA5; key[1] = seed[1] ^ 0xA5; key[2] = seed[2] ^ 0xA5; key[3] = seed[3] ^ 0xA5;
            break;
        case 7:
            cand_name = "XOR 0x55 (seed ^ 55555555)";
            key[0] = seed[0] ^ 0x55; key[1] = seed[1] ^ 0x55; key[2] = seed[2] ^ 0x55; key[3] = seed[3] ^ 0x55;
            break;
        case 8:
            cand_name = "VIN BCD (00 04 52 03)";
            key[0] = 0x00; key[1] = 0x04; key[2] = 0x52; key[3] = 0x03;
            break;
        case 9:
            cand_name = "VIN ASCII (34 35 32 33 -> '4523')";
            key[0] = 0x34; key[1] = 0x35; key[2] = 0x32; key[3] = 0x33;
            break;
        case 10:
            cand_name = "ASCII 0000 (30 30 30 30)";
            key[0] = 0x30; key[1] = 0x30; key[2] = 0x30; key[3] = 0x30;
            break;
        case 11:
            cand_name = "ASCII 1234 (31 32 33 34)";
            key[0] = 0x31; key[1] = 0x32; key[2] = 0x33; key[3] = 0x34;
            break;
        case 12:
            cand_name = "BCD 12345 (00 01 23 45)";
            key[0] = 0x00; key[1] = 0x01; key[2] = 0x23; key[3] = 0x45;
            break;
        case 13:
            cand_name = "Custom Hex Key";
            if (custom_key_hex && strlen(custom_key_hex) >= 8) {
                unsigned int b[4] = {0};
                if (sscanf(custom_key_hex, "%02x%02x%02x%02x", &b[0], &b[1], &b[2], &b[3]) == 4) {
                    key[0] = (uint8_t)b[0]; key[1] = (uint8_t)b[1]; key[2] = (uint8_t)b[2]; key[3] = (uint8_t)b[3];
                }
            }
            break;
        case 14: {
            cand_name = "ANSI C LCG (seed * 1103515245 + 12345)";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = s * 1103515245U + 12345U;
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 15:
            cand_name = "Byte Swap (seed endianness reversal)";
            key[0] = seed[3]; key[1] = seed[2]; key[2] = seed[1]; key[3] = seed[0];
            break;
        case 16:
            cand_name = "Nibble Swap";
            for (int i = 0; i < 4; i++) key[i] = (uint8_t)(((seed[i] & 0x0F) << 4) | (seed[i] >> 4));
            break;
        case 17:
            cand_name = "Inverted Byte Swap (~seed reversed)";
            key[0] = ~seed[3]; key[1] = ~seed[2]; key[2] = ~seed[1]; key[3] = ~seed[0];
            break;
        case 18: {
            cand_name = "Rotate Left 1 bit";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = (s << 1) | (s >> 31);
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 19: {
            cand_name = "Rotate Left 4 bits (1 nibble)";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = (s << 4) | (s >> 28);
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 20: {
            cand_name = "Rotate Left 8 bits (1 byte)";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = (s << 8) | (s >> 24);
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 21: {
            cand_name = "CRC32 ISO (poly 0xEDB88320)";
            uint32_t crc = 0xFFFFFFFF;
            for (int i = 0; i < 4; i++) {
                crc ^= seed[i];
                for (int j = 0; j < 8; j++) {
                    crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
                }
            }
            crc ^= 0xFFFFFFFF;
            key[0] = (crc >> 24) & 0xFF; key[1] = (crc >> 16) & 0xFF; key[2] = (crc >> 8) & 0xFF; key[3] = crc & 0xFF;
            break;
        }
        case 22: {
            cand_name = "Seed XOR 123 (seed ^ 0x0000007B)";
            key[0] = seed[0]; key[1] = seed[1]; key[2] = seed[2]; key[3] = seed[3] ^ 0x7B;
            break;
        }
        case 23: {
            cand_name = "Seed + 123";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = s + 123;
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 24:
            cand_name = "Fixed Key 00 00 00 01";
            key[0] = 0x00; key[1] = 0x00; key[2] = 0x00; key[3] = 0x01;
            break;
        case 25:
            cand_name = "Fixed Key FF FF FF FF";
            key[0] = 0xFF; key[1] = 0xFF; key[2] = 0xFF; key[3] = 0xFF;
            break;
        case 26:
            cand_name = "Fixed ASCII 'COBO' (43 4F 42 4F)";
            key[0] = 0x43; key[1] = 0x4F; key[2] = 0x42; key[3] = 0x4F;
            break;
        case 27:
            cand_name = "Fixed ASCII 'GUZZ' (47 55 5A 5A)";
            key[0] = 0x47; key[1] = 0x55; key[2] = 0x5A; key[3] = 0x5A;
            break;
        case 28: {
            cand_name = "Seed XOR BCD 123 (seed ^ 0x00000123)";
            key[0] = seed[0]; key[1] = seed[1]; key[2] = seed[2] ^ 0x01; key[3] = seed[3] ^ 0x23;
            break;
        }
        case 29: {
            cand_name = "Bosch/Conti LFSR (4 * ((s>>3)^s) ^ s)";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = (4 * ((s >> 3) ^ s)) ^ s;
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        case 30: {
            cand_name = "Marelli WordSwap XOR (swap16 ^ 55AA55AA)";
            uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) | ((uint32_t)seed[2] << 8) | seed[3];
            uint32_t k = (((s & 0xFF00FF00) >> 8) | ((s & 0x00FF00FF) << 8)) ^ 0x55AA55AA;
            key[0] = (k >> 24) & 0xFF; key[1] = (k >> 16) & 0xFF; key[2] = (k >> 8) & 0xFF; key[3] = k & 0xFF;
            break;
        }
        default:
            cand_name = "Echo (default)";
            key[0] = seed[0]; key[1] = seed[1]; key[2] = seed[2]; key[3] = seed[3];
            break;
    }

    ESP_LOGI(TAG, "SecTest [%s]: Seed %02X %02X %02X %02X -> Key %02X %02X %02X %02X",
             cand_name, seed[0], seed[1], seed[2], seed[3], key[0], key[1], key[2], key[3]);

    // Send Key
    uint8_t key_frame[8] = { 0x06, 0x27, 0x02, key[0], key[1], key[2], key[3], 0 };
    char k_hex[64], k_asc[32], k_stat[64];
    transceive_uds_isotp(&tgt, key_frame, 8, k_hex, sizeof(k_hex), k_asc, sizeof(k_asc), k_stat, sizeof(k_stat));

    bool unlocked = (k_hex[0] == '6' && k_hex[1] == '7');

    char w40_hex[64] = "NOT_EXECUTED", rb40_hex[64] = "NOT_EXECUTED";

    if (unlocked) {
        ESP_LOGI(TAG, "****************************************************************");
        ESP_LOGI(TAG, "*** SUCCESS! SECURITY ACCESS UNLOCKED VIA CANDIDATE %d (%s)! ***", cand_idx, cand_name);
        ESP_LOGI(TAG, "****************************************************************");

        // 1. Write LocalID 0x40 = 0x00 (Clear Maintenance Reminder Flag)
        uint8_t r_w40[8] = { 0x03, 0x3B, 0x40, 0x00, 0, 0, 0, 0 };
        char w_asc[32], w_stat[64];
        transceive_uds_isotp(&tgt, r_w40, 8, w40_hex, sizeof(w40_hex), w_asc, sizeof(w_asc), w_stat, sizeof(w_stat));
        ESP_LOGI(TAG, ">>> Write LocalID 0x40 [00] -> %s | %s <<<", w40_hex, w_stat);

        vTaskDelay(pdMS_TO_TICKS(50));

        // 2. Read back LocalID 0x40
        uint8_t r_rb40[8] = { 0x02, 0x21, 0x40, 0, 0, 0, 0, 0 };
        char rb_asc[32], rb_stat[64];
        transceive_uds_isotp(&tgt, r_rb40, 8, rb40_hex, sizeof(rb40_hex), rb_asc, sizeof(rb_asc), rb_stat, sizeof(rb_stat));
        ESP_LOGI(TAG, ">>> Readback LocalID 0x40 -> %s | %s <<<", rb40_hex, rb_stat);

        // 3. Clear DTCs
        uint8_t r_clr[8] = { 0x03, 0x14, 0xFF, 0x00, 0, 0, 0, 0 };
        char c_hex[64], c_asc[32], c_stat[64];
        transceive_uds_isotp(&tgt, r_clr, 8, c_hex, sizeof(c_hex), c_asc, sizeof(c_asc), c_stat, sizeof(c_stat));
    } else {
        ESP_LOGW(TAG, "SecTest Candidate %d (%s) REJECTED: %s | %s", cand_idx, cand_name, k_hex, k_stat);
    }

    if (out && out_len > 0) {
        snprintf(out, out_len,
            "{\"unlocked\":%s,\"candidate\":%d,\"name\":\"%s\","
            "\"seed\":\"%02X %02X %02X %02X\",\"key\":\"%02X %02X %02X %02X\","
            "\"sec_hex\":\"%s\",\"sec_status\":\"%s\","
            "\"write_40\":\"%s\",\"read_40\":\"%s\"}",
            unlocked ? "true" : "false", cand_idx, cand_name,
            seed[0], seed[1], seed[2], seed[3], key[0], key[1], key[2], key[3],
            k_hex, k_stat, w40_hex, rb40_hex);
    }

    log_twai_status("SecTest: Done");
    return unlocked ? ESP_OK : ESP_FAIL;
}
