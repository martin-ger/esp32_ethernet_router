idf_component_register(
    SRCS "netflow.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}/include"
    REQUIRES lwip freertos esp_timer nvs_flash log
)
