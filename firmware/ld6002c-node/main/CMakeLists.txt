idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_http_client esp_timer esp_driver_uart esp_driver_gpio
)
