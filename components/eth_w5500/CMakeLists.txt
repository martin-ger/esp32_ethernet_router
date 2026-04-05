idf_component_register(
    SRCS "w5500_spi_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_driver_spi heap freertos
)
