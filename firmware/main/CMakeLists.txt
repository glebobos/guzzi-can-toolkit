idf_component_register(
    SRCS "main.c" "wifi_ap.c" "http_server.c" "can_twai.c" "guzzi_dbc.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server spiffs driver dns_server esp_timer
)
