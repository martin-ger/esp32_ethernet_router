if(CONFIG_MQTT_HOMEASSISTANT)
    set(MQTT_HA_SRCS "mqtt_ha.c")
    set(MQTT_HA_REQS mqtt esp_event esp_wifi esp_netif esp_timer nvs_flash console cmd_router remote_console)
else()
    set(MQTT_HA_SRCS "")
    set(MQTT_HA_REQS "")
endif()

idf_component_register(
    SRCS ${MQTT_HA_SRCS}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${MQTT_HA_REQS}
)

if(CONFIG_MQTT_HOMEASSISTANT)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
endif()
