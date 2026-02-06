idf_component_register(
    SRCS
        "main.c"
        "serial_protocol.c"
        "wifi_controller.c"
        "http_relay.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_http_client
        nvs_flash
        json
        mbedtls
)
