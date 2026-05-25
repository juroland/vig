# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(vig)

# Fix compatibility issue
if(TARGET __idf_espressif__esp_ipa)
    target_compile_options(__idf_espressif__esp_ipa PRIVATE "-include" "stddef.h")
endif()

