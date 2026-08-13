#ifndef GUZZI_DBC_H
#define GUZZI_DBC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Engine & Performance
    uint16_t rpm;                 // BO_ 256 engine_speed (RPM)
    int16_t oil_temp_c;           // BO_ 640 TOILC_CAN (°C)
    int16_t coolant_temp_c;       // BO_ 512 TH2OC (°C)
    float battery_voltage;        // BO_ 512 VBATC (V)
    float fuel_consumption;       // BO_ 256 consumometro (L/100km)
    
    // Speed & Odometer
    float rear_speed_kmh;         // BO_ 608 WHEEL_SPEED_REAR (km/h)
    float front_speed_kmh;        // BO_ 608 front_speed (km/h)
    uint32_t odometer_km;         // BO_ 544 DSB_Odometer (km)
    uint16_t fuel_range_km;       // BO_ 544 DSB_FuelRange (km)
    uint8_t fuel_level_pct;       // BO_ 560 DSB_FuelLevel (%)
    
    // Rider Aids & Electronics
    uint8_t gear_position;        // BO_ 256 sensore_folle (0=N, 1-6=Gears)
    uint8_t traction_control;     // BO_ 260 TRACLEVEL (0-15)
    uint8_t abs_map;              // BO_ 609 ABSmap (0-15)
    uint8_t engine_map;           // BO_ 260 EngMap (0-4)
    uint8_t engine_brake_map;     // BO_ 260 EngBrakeMap
    
    // Dynamics & Hydraulics
    float accel_g;                // BO_ 630 AccX_IMUC (G)
    float front_brake_bar;        // BO_ 609 PressureFront (Bar)
    
    // Indicators & Alarms
    bool warning_lamp;            // BO_ 256 Warning_Lamp
    bool mil_lamp;                // BO_ 256 MIL Check Engine
    bool oil_alarm;               // BO_ 544 DSB_OilAlarm
    bool service_wrench;          // BO_ 256 URGENT_Service_E5 / E4
    
    uint32_t last_update_ms;
} guzzi_telemetry_t;

extern guzzi_telemetry_t g_telemetry;

#include "esp_err.h"

/**
 * Initializes DBC telemetry parser and mutex.
 */
esp_err_t guzzi_dbc_init(void);

/**
 * Parses incoming CAN frame ID and 8-byte payload based on Moto Guzzi DBC rules.
 */
void guzzi_dbc_parse_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/**
 * Returns telemetry data serialised as JSON string.
 */
int guzzi_telemetry_to_json(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // GUZZI_DBC_H
