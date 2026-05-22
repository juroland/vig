idf_component_register(SRCS "h264_encoder.cpp" "camera.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "esp_driver_cam" "esp_driver_isp" "esp_driver_i2c" "espressif__esp_h264"
    "esp_hw_support" "espressif__esp_cam_sensor" "efuse" "esp_timer"
    # intern
    "vig_common"
)
