idf_component_register(SRCS "src/vigo_telemetry.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "vigo_camera" "esp_driver_jpeg" "esp_timer" "mbedtls"
)
