idf_component_register(SRCS "cmd_router.c"
                    INCLUDE_DIRS . "${CMAKE_SOURCE_DIR}/include"
                    REQUIRES console nvs_flash esp_wifi driver spi_flash pcap_capture acl remote_console syslog mbedtls app_update espressif__iperf
                    PRIV_REQUIRES eth_w5500)
