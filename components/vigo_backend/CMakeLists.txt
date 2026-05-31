idf_component_register(SRCS "src/vigo_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "vigo_net" "cjson" "vigo_telemetry" "vigo_common"
)
