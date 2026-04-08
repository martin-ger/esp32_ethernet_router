idf_component_register(
    SRCS "w5500_spi_driver.c" "esp_eth_mac_w5500.c" "esp_eth_phy_w5500.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."          # for w5500.h (co-located with esp_eth_mac_w5500.c)
    REQUIRES esp_eth
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer log heap freertos
)

# Remove the upstream W5500 MAC and PHY drivers from esp_eth and replace with
# our patched versions (taskYIELD in TX-done loop, link-down debounce in PHY).
idf_component_get_property(esp_eth_lib esp_eth COMPONENT_LIB)
get_target_property(esp_eth_sources ${esp_eth_lib} SOURCES)
list(FILTER esp_eth_sources EXCLUDE REGEX "esp_eth_mac_w5500\\.c$")
list(FILTER esp_eth_sources EXCLUDE REGEX "esp_eth_phy_w5500\\.c$")
set_target_properties(${esp_eth_lib} PROPERTIES SOURCES "${esp_eth_sources}")
