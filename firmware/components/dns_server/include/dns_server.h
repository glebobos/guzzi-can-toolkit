#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start DNS Captive Portal server redirecting all DNS queries (*) to 192.168.4.1.
 */
esp_err_t start_dns_server(void);

/**
 * Stop DNS Captive Portal server.
 */
void stop_dns_server(void);

#ifdef __cplusplus
}
#endif

#endif // DNS_SERVER_H
