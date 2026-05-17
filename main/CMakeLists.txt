idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES esp_eth driver
)


target_compile_options(${COMPONENT_LIB} PRIVATE 
    -std=gnu++23
    -Wall 
    -Wextra 
    -Wpedantic 
    -Wconversion 
    -Werror=return-type
)