idf_component_register(SRCS "h264_encoder.cpp" "camera.cpp" "motion_detector.cpp"
    "pedestrian_detector.cpp" "surveillance_pipeline.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "esp_driver_cam" "esp_driver_isp" "esp_driver_i2c"
    "esp_hw_support" "espressif__esp_cam_sensor" "efuse" "esp_timer" "espressif__esp-dl"
    "espressif__pedestrian_detect" "esp_driver_ppa"
    REQUIRES "vigo_backend" "vigo_common" "espressif__esp_h264"
)
