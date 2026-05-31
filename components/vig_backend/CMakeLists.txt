idf_component_register(SRCS "src/vig_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "vig_net" "cjson" "vig_telemetry" "vig_common"
)
