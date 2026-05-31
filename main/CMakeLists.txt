set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "include"

    REQUIRES
    # extern components
    esp_eth esp_common driver nvs_flash
    # internal components
    vig_net
    vig_common
    vig_camera
    vig_stream_server
    vig_backend
    vig_telemetry
    vig_whip
)

# Inject the version string into the main component during build phase
if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VIG_VERSION=\"${PROJECT_VER}\")
endif()

add_compile_options(
    "-Wall" "-Wextra" "-Werror"
    "-Wconversion" "-Werror=return-type"
    "-Wno-unused-parameter")
