#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes SPIFFS and starts the Async HTTP Server with WebSockets and Captive Portal support.
 */
esp_err_t start_web_server(void);

/**
 * Stops the HTTP server.
 */
void stop_web_server(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
