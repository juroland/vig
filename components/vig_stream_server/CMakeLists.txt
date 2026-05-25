idf_component_register(SRCS "stream_server.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "esp_http_server" "freertos" "vig_camera" "vig_common")

# Custom command to generate stream_page.h from template and JMuxer
add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/include/stream_page.h"
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/embed_jmuxer.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stream_page.template.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend/libs/jmuxer_full.min.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stream_page.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/stream_page.template.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend/libs/jmuxer_full.min.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/embed_jmuxer.py"
        COMMENT "Embedding HTML and JMuxer into stream_page.h"
)

set_source_files_properties(stream_server.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/stream_page.h")
