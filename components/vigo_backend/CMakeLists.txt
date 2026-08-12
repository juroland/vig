idf_component_register(SRCS "src/vigo_backend.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "vigo_net" "cjson" "vigo_common"
    REQUIRES "vigo_telemetry"
)
