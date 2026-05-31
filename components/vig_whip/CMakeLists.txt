idf_component_register(SRCS "src/vig_whip.cpp"
    INCLUDE_DIRS "include"
    REQUIRES vig_net vig_camera vig_common lwip mbedtls esp_hw_support esp_http_client
)
