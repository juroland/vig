idf_component_register(SRCS "src/vigo_whip.cpp"
    INCLUDE_DIRS "include"
    REQUIRES vigo_net vigo_camera vigo_common lwip mbedtls esp_hw_support esp_http_client
)
