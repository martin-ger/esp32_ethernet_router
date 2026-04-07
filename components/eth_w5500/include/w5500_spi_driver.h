#pragma once
/*
 * Custom W5500 SPI driver with a pre-allocated DMA-aligned TX buffer.
 *
 * Root cause of the "Load access fault" crash: lwIP packet buffers are not
 * guaranteed to be DMA-aligned, so the SPI master's setup_dma_priv_buffer()
 * allocates a per-frame DMA bounce buffer from the heap on every TX call.
 * Under sustained load this exhausts the DMA heap, the allocation fails,
 * the driver's error recovery dereferences a NULL pointer, and the system
 * crashes. By pre-allocating one aligned DMA buffer here and reusing it for
 * every write, the per-frame heap allocation is eliminated entirely.
 *
 * The TX path is serialised by the SPI mutex, so a single shared buffer is safe.
 *
 * Usage: populate eth_w5500_config_t.custom_spi_driver with
 *   w5500_spi_driver_config() before calling esp_eth_mac_new_w5500().
 */

#include "sdkconfig.h"

#if defined(CONFIG_ETH_DOWNLINK_W5500)

#include "esp_eth_mac_spi.h"

/**
 * Fill an eth_spi_custom_driver_config_t for use with esp_eth_mac_new_w5500().
 *
 * @param driver  Pointer to the config struct to populate.
 * @param w5500   Pointer to the eth_w5500_config_t that holds spi_host_id and
 *                spi_devcfg. Must remain valid until esp_eth_mac_new_w5500()
 *                returns (it is only accessed during the synchronous init call).
 */
void w5500_spi_driver_config(eth_spi_custom_driver_config_t *driver,
                              const eth_w5500_config_t *w5500);

/** SPI error counters — incremented from IRAM context, read from task context. */
typedef struct {
    uint32_t read_spi_fail;     /**< spi_device_polling_transmit errors on read */
    uint32_t read_timeout;      /**< mutex timeout on read */
    uint32_t write_spi_fail;    /**< spi_device_polling_transmit errors on write */
    uint32_t write_timeout;     /**< mutex timeout on write */
    uint32_t write_zero_copy;   /**< TX frames sent without bounce-buffer copy */
    uint32_t write_bounce;      /**< TX frames that required a copy into tx_dma_buf */
} w5500_spi_stats_t;

/** Returns a snapshot of the current error counters. */
w5500_spi_stats_t w5500_spi_get_stats(void);

#endif // CONFIG_ETH_DOWNLINK_W5500
