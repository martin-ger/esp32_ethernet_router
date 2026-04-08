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
} w5500_spi_stats_t;

/** Returns a snapshot of the current error counters. */
w5500_spi_stats_t w5500_spi_get_stats(void);

/**
 * W5500 register snapshot for diagnostics.
 * All pointer registers are in host byte order.
 */
typedef struct {
    uint8_t  version;   /**< Chip version register (expect 0x04) */
    uint8_t  phycfgr;   /**< PHY config: bit0=LNK bit1=SPD bit2=DPX */
    uint8_t  simr;      /**< Socket interrupt mask register */
    uint8_t  sock_mr;   /**< Socket 0 mode (expect 0x04 = MAC_RAW) */
    uint8_t  sock_sr;   /**< Socket 0 status (expect 0x42 = SOCK_MACRAW) */
    uint8_t  sock_ir;   /**< Socket 0 pending interrupt flags */
    uint8_t  sock_imr;  /**< Socket 0 interrupt mask */
    uint16_t tx_fsr;    /**< TX free size in bytes (0 = stuck, max = 0x4000) */
    uint16_t tx_rd;     /**< TX read pointer */
    uint16_t tx_wr;     /**< TX write pointer */
    uint16_t rx_rsr;    /**< RX received size in bytes (pending RX data) */
    uint16_t rx_rd;     /**< RX read pointer */
    uint16_t rx_wr;     /**< RX write pointer */
} w5500_diag_t;

/**
 * Read a diagnostic snapshot from the W5500 via SPI.
 * Safe to call from any task; SPI access is mutex-protected.
 * Returns ESP_ERR_INVALID_STATE if the MAC has not been initialised yet.
 */
esp_err_t w5500_get_diag(w5500_diag_t *out);

/**
 * Soft-reset the W5500 socket layer without disturbing lwIP or NAT state.
 * Issues CLOSE → setup_default → OPEN → enables SIMR, then reads back SOCK_SR
 * to verify the socket is open (0x42 = MACRAW).
 *
 * Use this for manual recovery from a stuck socket.  Do NOT use esp_eth_stop/
 * esp_eth_start for this — those fire link-down/up events and tear down DHCP
 * and NAT state.
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_STATE if the chip did not respond
 * (e.g. mid-hardware-reset), or ESP_FAIL on SPI error.
 */
esp_err_t w5500_reset_socket(void);

#endif // CONFIG_ETH_DOWNLINK_W5500
