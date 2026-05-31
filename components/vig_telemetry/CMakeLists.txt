idf_component_register(SRCS "src/vig_telemetry.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "vig_camera" "esp_driver_jpeg" "esp_timer" "mbedtls"
)
