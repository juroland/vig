idf_component_register(SRCS "audio.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio "vigo_common"
    REQUIRES esp_driver_i2s
)
