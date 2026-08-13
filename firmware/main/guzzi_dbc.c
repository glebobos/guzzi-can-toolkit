#include "guzzi_dbc.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

guzzi_telemetry_t g_telemetry = {0};

static uint32_t get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void guzzi_dbc_parse_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc) {
    if (dlc < 8) return;

    g_telemetry.last_update_ms = get_time_ms();

    switch (can_id) {
        case 256: { // 0x100 ECU1
            // engine_speed: 31|16@0+ (1,0) -> B3:B4
            g_telemetry.rpm = ((uint16_t)data[3] << 8) | data[4];
            // consumometro: 23|8@0+ (0.2,0) -> B2
            g_telemetry.fuel_consumption = data[2] * 0.2f;
            // sensore_folle: 7|4@0+ -> B0 low nibble
            g_telemetry.gear_position = data[0] & 0x0F;
            // MIL: 43|2@0+
            g_telemetry.mil_lamp = ((data[5] >> 3) & 0x03) != 0;
            // Warning_Lamp: 46|1@0+
            g_telemetry.warning_lamp = ((data[5] >> 6) & 0x01) != 0;
            // URGENT_Service_E5: 47|1@0+
            g_telemetry.service_wrench = ((data[5] >> 7) & 0x01) != 0;
            break;
        }

        case 512: { // 0x200 ECU2 / MIU_0x200
            // TH2OC: 7|8@0+ (1,-40) -> B0
            g_telemetry.coolant_temp_c = (int16_t)data[0] - 40;
            // battery_voltage: 47|8@0+ (0.1,0) -> B5
            g_telemetry.battery_voltage = data[5] * 0.1f;
            break;
        }

        case 544: { // 0x220 DSB1
            // DSB_Odometer: 7|24@0+ -> B0, B1, B2
            g_telemetry.odometer_km = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
            // DSB_FuelRange: 2|11@0+ -> B3, B4
            g_telemetry.fuel_range_km = (((uint16_t)(data[3] & 0x07)) << 8) | data[4];
            // DSB_OilAlarm: 51|1@0+
            g_telemetry.oil_alarm = ((data[6] >> 3) & 0x01) != 0;
            break;
        }

        case 560: { // 0x230 DSB2
            // DSB_FuelLevel: 63|4@0+ -> B7 high nibble
            g_telemetry.fuel_level_pct = (data[7] >> 4) & 0x0F;
            break;
        }

        case 608: { // 0x260 ABS1
            // front_speed: 7|16@0+ (0.01,0) -> B0:B1
            g_telemetry.front_speed_kmh = (((uint16_t)data[0] << 8) | data[1]) * 0.01f;
            // WHEEL_SPEED_REAR: 23|16@0+ (0.01,0) -> B2:B3
            g_telemetry.rear_speed_kmh = (((uint16_t)data[2] << 8) | data[3]) * 0.01f;
            break;
        }

        case 609: { // 0x261 ABS2
            // ABSmap: 7|4@0+ -> B0 high nibble
            g_telemetry.abs_map = (data[0] >> 4) & 0x0F;
            // PressureFront: 15|16@0- (0.0153,0) -> B1:B2
            int16_t press_raw = (int16_t)(((uint16_t)data[1] << 8) | data[2]);
            g_telemetry.front_brake_bar = press_raw * 0.0153f;
            break;
        }

        case 640: { // 0x280 Engine5
            // TOILC_CAN: 23|8@0+ (1,-40) -> B2
            g_telemetry.oil_temp_c = (int16_t)data[2] - 40;
            break;
        }

        case 260: { // 0x104 ECU_0x104
            // TRACLEVEL: 7|4@0+ -> B0 high nibble
            g_telemetry.traction_control = (data[0] >> 4) & 0x0F;
            // EngBrakeMap: 19|4@0+ -> B2 low nibble
            g_telemetry.engine_brake_map = data[2] & 0x0F;
            // EngMap: 23|4@0+ -> B2 high nibble
            g_telemetry.engine_map = (data[2] >> 4) & 0x0F;
            break;
        }

        case 630: { // 0x276 IMU_0x276
            // AccX_IMUC: 0|12@1+ (0.00254,-5.20192) -> Little endian
            uint16_t raw_accel = ((uint16_t)data[0]) | (((uint16_t)(data[1] & 0x0F)) << 8);
            g_telemetry.accel_g = (raw_accel * 0.00254f) - 5.20192f;
            break;
        }

        default:
            break;
    }
}

int guzzi_telemetry_to_json(char *buf, size_t buf_len) {
    return snprintf(buf, buf_len,
        "{"
        "\"rpm\":%u,"
        "\"speed_kmh\":%.1f,"
        "\"gear\":%u,"
        "\"oil_temp_c\":%d,"
        "\"coolant_temp_c\":%d,"
        "\"battery_voltage\":%.1f,"
        "\"odometer_km\":%lu,"
        "\"fuel_range_km\":%u,"
        "\"fuel_level_pct\":%u,"
        "\"mgct_level\":%u,"
        "\"abs_mode\":%u,"
        "\"riding_mode\":%u,"
        "\"front_brake_bar\":%.1f,"
        "\"accel_g\":%.2f,"
        "\"mil_active\":%s,"
        "\"warning_active\":%s,"
        "\"oil_alarm_active\":%s,"
        "\"service_wrench_active\":%s,"
        "\"timestamp\":%lu"
        "}",
        g_telemetry.rpm,
        g_telemetry.rear_speed_kmh,
        g_telemetry.gear_position,
        g_telemetry.oil_temp_c,
        g_telemetry.coolant_temp_c,
        g_telemetry.battery_voltage,
        (unsigned long)g_telemetry.odometer_km,
        g_telemetry.fuel_range_km,
        g_telemetry.fuel_level_pct,
        g_telemetry.traction_control,
        g_telemetry.abs_map,
        g_telemetry.engine_map,
        g_telemetry.front_brake_bar,
        g_telemetry.accel_g,
        g_telemetry.mil_lamp ? "true" : "false",
        g_telemetry.warning_lamp ? "true" : "false",
        g_telemetry.oil_alarm ? "true" : "false",
        g_telemetry.service_wrench ? "true" : "false",
        (unsigned long)g_telemetry.last_update_ms
    );
}
