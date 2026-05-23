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
)

add_compile_options(
    "-Wall" "-Wextra" "-Werror"
    "-Wconversion" "-Werror=return-type"
    "-Wno-unused-parameter")
