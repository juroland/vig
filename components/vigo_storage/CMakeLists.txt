idf_component_register(SRCS "vigo_storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fatfs esp_adc
    PRIV_REQUIRES esp_tinyusb
    # intern
    "vigo_common"
)
