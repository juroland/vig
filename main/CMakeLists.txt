set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "include"

    REQUIRES
    # extern components
    esp_eth esp_common driver nvs_flash esp_wifi_remote esp_hosted
    # internal components
    vigo_audio
    vigo_led
    vigo_net
    vigo_common
    vigo_camera
    vigo_stream_server
    vigo_backend
    vigo_telemetry
    vigo_whip
    vigo_ota
    vigo_factory
)

# Inject the version string into the main component during build phase
if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIGO_VERSION=\"${PROJECT_VER}\")
endif()

add_compile_options(
    "-Wall" "-Wextra" "-Werror"
    "-Wconversion" "-Werror=return-type"
    "-Wno-unused-parameter")
