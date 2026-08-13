#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes Wi-Fi SoftAP access point and Captive Portal DNS server.
 */
esp_err_t wifi_init_softap(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_AP_H
