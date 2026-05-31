idf_component_register(
    SRCS "net.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "esp_eth" "esp_http_client" "esp_netif" "esp_driver_gpio" "mbedtls"
             "esp_wifi"
    # intern
    "vigo_common"
)
