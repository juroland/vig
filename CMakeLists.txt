# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

if(DEFINED DEVICE)
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/configs/sdkconfig.${DEVICE}")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;configs/${DEVICE}.defaults")
    message(STATUS "Build profile: Loading configs/${DEVICE}.defaults")
    message(STATUS "Build profile: Using sdkconfig path: configs/sdkconfig.${DEVICE}")
elseif(DEFINED ENV{DEVICE})
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/configs/sdkconfig.$ENV{DEVICE}")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;configs/$ENV{DEVICE}.defaults")
    message(STATUS "Build profile: Loading configs/$ENV{DEVICE}.defaults")
    message(STATUS "Build profile: Using sdkconfig path: configs/sdkconfig.$ENV{DEVICE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(vigo)

# Fix compatibility issue
if(TARGET __idf_espressif__esp_ipa)
    target_compile_options(__idf_espressif__esp_ipa PRIVATE "-include" "stddef.h")
endif()

