/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include <inttypes.h>
#include "esp_eth_mac_spi.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/io_mux_reg.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "w5500.h"
#include "w5500_spi_driver.h"
#include "sdkconfig.h"

static const char *TAG = "w5500.mac";

#define W5500_SPI_LOCK_TIMEOUT_MS (9999) 
#define W5500_TX_MEM_SIZE (0x4000)
#define W5500_RX_MEM_SIZE (0x4000)
#define W5500_100M_TX_TMO_US (2000)
#define W5500_10M_TX_TMO_US  (1500)
#define W5500_TX_DONE_TMO_MS  (10)   // SPI frame write ~480 µs + W5500 MAC + ISR + sched slack
#define W5500_TX_QUEUE_DEPTH  (4)    // max frames queued between lwIP and the TX task
#define W5500_TX_INLINE_THRESHOLD  (128)  // frames <= this bypass the pool/queue when TX is idle
#define W5500_SPI_POLLING_THRESHOLD (64)  // SPI transfers > this use DMA-async (spi_device_transmit)
#define W5500_ETH_MAC_RX_BUF_SIZE_AUTO (0)

typedef struct {
    uint8_t  *data;
    uint32_t  len;
} w5500_tx_frame_t;

typedef struct {
    uint32_t offset;
    uint32_t copy_len;
    uint32_t rx_len;
    uint32_t remain;
}__attribute__((packed)) emac_w5500_auto_buf_info_t;

typedef struct {
    spi_device_handle_t hdl;
    SemaphoreHandle_t lock;
} eth_spi_info_t;

typedef struct {
    void *ctx;
    void *(*init)(const void *spi_config);
    esp_err_t (*deinit)(void *spi_ctx);
    esp_err_t (*read)(void *spi_ctx, uint32_t cmd,uint32_t addr, void *data, uint32_t data_len);
    esp_err_t (*write)(void *spi_ctx, uint32_t cmd, uint32_t addr, const void *data, uint32_t data_len);
} eth_spi_custom_driver_t;

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_mediator_t *eth;
    eth_spi_custom_driver_t spi;
    TaskHandle_t rx_task_hdl;
    uint32_t sw_reset_timeout_ms;
    int int_gpio_num;
    esp_timer_handle_t poll_timer;
    uint32_t poll_period_ms;
    uint8_t addr[ETH_ADDR_LEN];
    bool packets_remain;
    uint8_t *rx_buffer;
    uint8_t mcast_cnt;
    uint32_t tx_tmo;
    QueueHandle_t     tx_queue;                       // pending frames: lwIP thread → emac task
    QueueHandle_t     tx_pool;                        // free TX frame buffer pool
    uint8_t          *tx_pool_bufs[W5500_TX_QUEUE_DEPTH]; // backing DMA memory for pool
    uint8_t          *tx_active_buf;                  // pool buffer in-flight (NULL for inline TX or when idle)
    uint64_t          tx_start_us;                    // esp_timer_get_time() when SEND was issued
    SemaphoreHandle_t tx_idle_sem;                    // binary sem (count=1 when TX idle, 0 when SEND in flight)
} emac_w5500_t;

/* Single-instance pointer used by w5500_get_diag() */
static emac_w5500_t *s_emac = NULL;

static void *w5500_spi_init(const void *spi_config)
{
    void *ret = NULL;
    eth_w5500_config_t *w5500_config = (eth_w5500_config_t *)spi_config;
    eth_spi_info_t *spi = calloc(1, sizeof(eth_spi_info_t));
    ESP_GOTO_ON_FALSE(spi, NULL, err, TAG, "no memory for SPI context data");

    /* SPI device init */
    spi_device_interface_config_t spi_devcfg;
    spi_devcfg = *(w5500_config->spi_devcfg);
    if (w5500_config->spi_devcfg->command_bits == 0 && w5500_config->spi_devcfg->address_bits == 0) {
        /* configure default SPI frame format */
        spi_devcfg.command_bits = 16; // Actually it's the address phase in W5500 SPI frame
        spi_devcfg.address_bits = 8;  // Actually it's the control phase in W5500 SPI frame
    } else {
        ESP_GOTO_ON_FALSE(w5500_config->spi_devcfg->command_bits == 16 && w5500_config->spi_devcfg->address_bits == 8,
                            NULL, err, TAG, "incorrect SPI frame format (command_bits/address_bits)");
    }
    ESP_GOTO_ON_FALSE(spi_bus_add_device(w5500_config->spi_host_id, &spi_devcfg, &spi->hdl) == ESP_OK, NULL,
                                            err, TAG, "adding device to SPI host #%i failed", w5500_config->spi_host_id + 1);
    /* create mutex */
    spi->lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(spi->lock, NULL, err, TAG, "create lock failed");

    ret = spi;
    return ret;
err:
    if (spi) {
        if (spi->lock) {
            vSemaphoreDelete(spi->lock);
        }
        free(spi);
    }
    return ret;
}

static esp_err_t w5500_spi_deinit(void *spi_ctx)
{
    esp_err_t ret = ESP_OK;
    eth_spi_info_t *spi = (eth_spi_info_t *)spi_ctx;

    spi_bus_remove_device(spi->hdl);
    vSemaphoreDelete(spi->lock);

    free(spi);
    return ret;
}

static inline bool w5500_spi_lock(eth_spi_info_t *spi)
{
    return xSemaphoreTake(spi->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static inline bool w5500_spi_unlock(eth_spi_info_t *spi)
{
    return xSemaphoreGive(spi->lock) == pdTRUE;
}

// Short transfers (registers, command writes) use polling — ISR/context-switch
// overhead outweighs DMA for < ~64 B.  Long transfers (frame data) use the
// interrupt-driven path so the calling task sleeps during the ~300 µs DMA
// instead of busy-waiting the single C3 core.
static esp_err_t w5500_spi_write(void *spi_ctx, uint32_t cmd, uint32_t addr, const void *value, uint32_t len)
{
    esp_err_t ret = ESP_OK;
    eth_spi_info_t *spi = (eth_spi_info_t *)spi_ctx;

    spi_transaction_t trans = {
        .cmd = cmd,
        .addr = addr,
        .length = 8 * len,
        .tx_buffer = value
    };
    if (w5500_spi_lock(spi)) {
        esp_err_t tx_err = (len > W5500_SPI_POLLING_THRESHOLD)
            ? spi_device_transmit(spi->hdl, &trans)
            : spi_device_polling_transmit(spi->hdl, &trans);
        if (tx_err != ESP_OK) {
            ESP_LOGE(TAG, "%s(%d): spi transmit failed", __FUNCTION__, __LINE__);
            ret = ESP_FAIL;
        }
        w5500_spi_unlock(spi);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static esp_err_t w5500_spi_read(void *spi_ctx, uint32_t cmd, uint32_t addr, void *value, uint32_t len)
{
    esp_err_t ret = ESP_OK;
    eth_spi_info_t *spi = (eth_spi_info_t *)spi_ctx;

    bool use_rxdata = (len <= 4);
    spi_transaction_t trans = {
        .flags = use_rxdata ? SPI_TRANS_USE_RXDATA : 0, // use direct reads for registers to prevent overwrites by 4-byte boundary writes
        .cmd = cmd,
        .addr = addr,
        .length = 8 * len,
        .rx_buffer = value
    };
    if (w5500_spi_lock(spi)) {
        esp_err_t tx_err = (!use_rxdata && len > W5500_SPI_POLLING_THRESHOLD)
            ? spi_device_transmit(spi->hdl, &trans)
            : spi_device_polling_transmit(spi->hdl, &trans);
        if (tx_err != ESP_OK) {
            ESP_LOGE(TAG, "%s(%d): spi transmit failed", __FUNCTION__, __LINE__);
            ret = ESP_FAIL;
        }
        w5500_spi_unlock(spi);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    if (use_rxdata) {
        memcpy(value, trans.rx_data, len);  // copy register values to output
    }
    return ret;
}

static esp_err_t w5500_read(emac_w5500_t *emac, uint32_t address, void *data, uint32_t len)
{
    uint32_t cmd = (address >> W5500_ADDR_OFFSET); // Actually it's the address phase in W5500 SPI frame
    uint32_t addr = ((address & 0xFFFF) | (W5500_ACCESS_MODE_READ << W5500_RWB_OFFSET)
                    | W5500_SPI_OP_MODE_VDM); // Actually it's the command phase in W5500 SPI frame

    return emac->spi.read(emac->spi.ctx, cmd, addr, data, len);
}

static esp_err_t w5500_write(emac_w5500_t *emac, uint32_t address, const void *data, uint32_t len)
{
    uint32_t cmd = (address >> W5500_ADDR_OFFSET); // Actually it's the address phase in W5500 SPI frame
    uint32_t addr = ((address & 0xFFFF) | (W5500_ACCESS_MODE_WRITE << W5500_RWB_OFFSET)
                    | W5500_SPI_OP_MODE_VDM); // Actually it's the command phase in W5500 SPI frame

    return emac->spi.write(emac->spi.ctx, cmd, addr, data, len);
}

// Fire-and-forget SEND on SOCK0: writes SCR and returns without reading it
// back.  The chip auto-clears SCR within microseconds of accepting the
// command, but the readback costs a full SPI round-trip we don't need on the
// TX hot path — SIR_SEND is the authoritative completion signal, caught
// asynchronously by the emac task.  Safe because tx_idle_sem serializes
// SENDs: the next SEND is only issued after SIR_SEND has fired for the
// previous one, by which point SCR is long cleared.
static inline esp_err_t w5500_issue_send(emac_w5500_t *emac)
{
    uint8_t cmd = W5500_SCR_SEND;
    return w5500_write(emac, W5500_REG_SOCK_CR(0), &cmd, sizeof(cmd));
}

static esp_err_t w5500_send_command(emac_w5500_t *emac, uint8_t command, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_CR(0), &command, sizeof(command)), err, TAG, "write SCR failed");
    // after W5500 accepts the command, the command register will be cleared automatically
    uint32_t to = 0;
    for (to = 0; to < timeout_ms / 10; to++) {
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_CR(0), &command, sizeof(command)), err, TAG, "read SCR failed");
        if (!command) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_GOTO_ON_FALSE(to < timeout_ms / 10, ESP_ERR_TIMEOUT, err, TAG, "send command timeout");

err:
    return ret;
}

static esp_err_t w5500_get_tx_free_size(emac_w5500_t *emac, uint16_t *size)
{
    esp_err_t ret = ESP_OK;
    uint16_t free0, free1 = 0;
    // read TX_FSR register more than once, until we get the same value
    // this is a trick because we might be interrupted between reading the high/low part of the TX_FSR register (16 bits in length)
    do {
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_TX_FSR(0), &free0, sizeof(free0)), err, TAG, "read TX FSR failed");
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_TX_FSR(0), &free1, sizeof(free1)), err, TAG, "read TX FSR failed");
    } while (free0 != free1);

    *size = __builtin_bswap16(free0);

err:
    return ret;
}

static esp_err_t w5500_get_rx_received_size(emac_w5500_t *emac, uint16_t *size)
{
    esp_err_t ret = ESP_OK;
    uint16_t received0, received1 = 0;
    do {
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_RX_RSR(0), &received0, sizeof(received0)), err, TAG, "read RX RSR failed");
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_RX_RSR(0), &received1, sizeof(received1)), err, TAG, "read RX RSR failed");
    } while (received0 != received1);
    *size = __builtin_bswap16(received0);

err:
    return ret;
}

static esp_err_t w5500_write_buffer(emac_w5500_t *emac, const void *buffer, uint32_t len, uint16_t offset)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_MEM_SOCK_TX(0, offset), buffer, len), err, TAG, "write TX buffer failed");
err:
    return ret;
}

static esp_err_t w5500_read_buffer(emac_w5500_t *emac, void *buffer, uint32_t len, uint16_t offset)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_MEM_SOCK_RX(0, offset), buffer, len), err, TAG, "read RX buffer failed");
err:
    return ret;
}

static esp_err_t w5500_set_mac_addr(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_MAC, emac->addr, 6), err, TAG, "write MAC address register failed");
err:
    return ret;
}

static esp_err_t w5500_reset(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    /* software reset */
    uint8_t mr = W5500_MR_RST; // Set RST bit (auto clear)
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_MR, &mr, sizeof(mr)), err, TAG, "write MR failed");
    uint32_t to = 0;
    for (to = 0; to < emac->sw_reset_timeout_ms / 10; to++) {
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_MR, &mr, sizeof(mr)), err, TAG, "read MR failed");
        if (!(mr & W5500_MR_RST)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_GOTO_ON_FALSE(to < emac->sw_reset_timeout_ms / 10, ESP_ERR_TIMEOUT, err, TAG, "reset timeout");

err:
    return ret;
}

static esp_err_t w5500_verify_id(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint8_t version = 0;

    // W5500 doesn't have chip ID, we check the version number instead
    // The version number may be polled multiple times since it was observed that
    // some W5500 units may return version 0 when it is read right after the reset
    ESP_LOGD(TAG, "Waiting W5500 to start & verify version...");
    uint32_t to = 0;
    for (to = 0; to < emac->sw_reset_timeout_ms / 10; to++) {
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_VERSIONR, &version, sizeof(version)), err, TAG, "read VERSIONR failed");
        if (version == W5500_CHIP_VERSION) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "W5500 version mismatched, expected 0x%02x, got 0x%02" PRIx8, W5500_CHIP_VERSION, version);
    return ESP_ERR_INVALID_VERSION;
err:
    return ret;
}

static esp_err_t w5500_setup_default(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint8_t reg_value = 16;

    // Only SOCK0 can be used as MAC RAW mode, so we give the whole buffer (16KB TX and 16KB RX) to SOCK0, which doesn't have any effect for TX though.
    // A larger TX buffer doesn't buy us pipelining - each SEND is one frame and must complete before the next.
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_RXBUF_SIZE(0), &reg_value, sizeof(reg_value)), err, TAG, "set rx buffer size failed");
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_TXBUF_SIZE(0), &reg_value, sizeof(reg_value)), err, TAG, "set tx buffer size failed");
    reg_value = 0;
    for (int i = 1; i < 8; i++) {
        ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_RXBUF_SIZE(i), &reg_value, sizeof(reg_value)), err, TAG, "set rx buffer size failed");
        ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_TXBUF_SIZE(i), &reg_value, sizeof(reg_value)), err, TAG, "set tx buffer size failed");
    }

    /* Restore MAC address — W5500_REG_MAC resets to 0 on hardware power-glitch.
     * With MAC_FILTER enabled the chip silently drops all unicast frames that
     * don't match this register, so it must be restored before opening SOCK0. */
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_MAC, emac->addr, ETH_ADDR_LEN), err, TAG, "write MAC address failed");
    /* Enable ping block, disable PPPoE, WOL */
    reg_value = W5500_MR_PB;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_MR, &reg_value, sizeof(reg_value)), err, TAG, "write MR failed");
    /* Disable interrupt for all sockets by default */
    reg_value = 0;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SIMR, &reg_value, sizeof(reg_value)), err, TAG, "write SIMR failed");
    /* Enable MAC RAW mode for SOCK0, enable MAC filter, no blocking broadcast and block multicast */
    reg_value = W5500_SMR_MAC_RAW | W5500_SMR_MAC_FILTER | W5500_SMR_MAC_BLOCK_MCAST;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_MR(0), &reg_value, sizeof(reg_value)), err, TAG, "write SMR failed");
    /* Enable receive and send-done events for SOCK0 */
    reg_value = W5500_SIR_RECV | W5500_SIR_SEND;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_IMR(0), &reg_value, sizeof(reg_value)), err, TAG, "write SOCK0 IMR failed");
    /* Set the interrupt re-assert level to maximum (~1.5ms) to lower the chances of missing it */
    uint16_t int_level = __builtin_bswap16(0xFFFF);
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_INTLEVEL, &int_level, sizeof(int_level)), err, TAG, "write INTLEVEL failed");

err:
    return ret;
}

// Re-open SOCK0 in MAC-RAW mode and verify SOCK_SR reads back 0x42.
// Returns ESP_OK on success, ESP_ERR_INVALID_STATE if the chip didn't respond
// (e.g. still in hardware-reset recovery), ESP_FAIL on SPI error.
static esp_err_t w5500_reopen_socket(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_CLOSE, 100), err, TAG, "reopen: CLOSE failed");
    ESP_GOTO_ON_ERROR(w5500_setup_default(emac), err, TAG, "reopen: setup_default failed");
    ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_OPEN, 100), err, TAG, "reopen: OPEN failed");
    uint8_t simr = W5500_SIMR_SOCK0;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SIMR, &simr, sizeof(simr)), err, TAG, "reopen: write SIMR failed");
    // Verify the socket actually opened — if W5500 is still in hardware-reset
    // the command register clears immediately (reads 0x00 by default) but the
    // socket stays closed (SR=0x00 instead of 0x42=MACRAW).
    uint8_t sr = 0;
    ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_SR(0), &sr, sizeof(sr)), err, TAG, "reopen: read SR failed");
    if (sr != W5500_SSR_MACRAW) {
        ESP_LOGW(TAG, "W5500 socket reopen failed (SR=0x%02X, expected 0x%02X) — chip may still be resetting",
                 sr, W5500_SSR_MACRAW);
        ret = ESP_ERR_INVALID_STATE;
    }
err:
    return ret;
}

static esp_err_t emac_w5500_start(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    // Re-apply default register setup before opening the socket.
    // A real link-down event is often caused by a W5500 power glitch that resets
    // all its registers to defaults (TCP socket mode, 2KB buffers, etc.).
    // Without this, SOCK0 opens in TCP mode and SEND commands never complete.
    ESP_GOTO_ON_ERROR(w5500_setup_default(emac), err, TAG, "w5500 setup default failed");
    /* open SOCK0 */
    ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_OPEN, 100), err, TAG, "issue OPEN command failed");
    /* enable interrupt for SOCK0 */
    uint8_t reg_value = W5500_SIMR_SOCK0;
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SIMR, &reg_value, sizeof(reg_value)), err, TAG, "write SIMR failed");

err:
    return ret;
}

static esp_err_t emac_w5500_stop(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    uint8_t reg_value = 0;
    /* disable interrupt */
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SIMR, &reg_value, sizeof(reg_value)), err, TAG, "write SIMR failed");
    /* close SOCK0 */
    ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_CLOSE, 100), err, TAG, "issue CLOSE command failed");

err:
    return ret;
}

static esp_err_t emac_w5500_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(eth, ESP_ERR_INVALID_ARG, err, TAG, "can't set mac's mediator to null");
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    emac->eth = eth;
    return ESP_OK;
err:
    return ret;
}

static esp_err_t emac_w5500_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    // PHY register and MAC registers are mixed together in W5500
    // The only PHY register is PHYCFGR
    ESP_GOTO_ON_FALSE(phy_reg == W5500_REG_PHYCFGR, ESP_FAIL, err, TAG, "wrong PHY register");
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_PHYCFGR, &reg_value, sizeof(uint8_t)), err, TAG, "write PHY register failed");

err:
    return ret;
}

static esp_err_t emac_w5500_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(reg_value, ESP_ERR_INVALID_ARG, err, TAG, "can't set reg_value to null");
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    // PHY register and MAC registers are mixed together in W5500
    // The only PHY register is PHYCFGR
    ESP_GOTO_ON_FALSE(phy_reg == W5500_REG_PHYCFGR, ESP_FAIL, err, TAG, "wrong PHY register");
    ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_PHYCFGR, reg_value, sizeof(uint8_t)), err, TAG, "read PHY register failed");

err:
    return ret;
}

static esp_err_t emac_w5500_set_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    memcpy(emac->addr, addr, 6);
    ESP_GOTO_ON_ERROR(w5500_set_mac_addr(emac), err, TAG, "set mac address failed");

err:
    return ret;
}

static esp_err_t emac_w5500_get_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    memcpy(addr, emac->addr, 6);

err:
    return ret;
}

static esp_err_t emac_w5500_set_block_ip4_mcast(esp_eth_mac_t *mac, bool block)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    uint8_t smr;
    ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_MR(0), &smr, sizeof(smr)), err, TAG, "read SMR failed");
    if (block) {
        smr |= W5500_SMR_MAC_BLOCK_MCAST;
    } else {
        smr &= ~W5500_SMR_MAC_BLOCK_MCAST;
    }
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_MR(0), &smr, sizeof(smr)), err, TAG, "write SMR failed");
err:
    return ret;
}

static esp_err_t emac_w5500_add_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    // W5500 doesn't have specific MAC filter, so we just un-block multicast. W5500 filters out all multicast packets
    // except for IP multicast. However, behavior is not consistent. IPv4 multicast can be blocked, but IPv6 is always
    // accepted (this is not documented behavior, but it's observed on the real hardware).
    if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e) {
        ESP_GOTO_ON_ERROR(emac_w5500_set_block_ip4_mcast(mac, false), err, TAG, "set block multicast failed");
        emac->mcast_cnt++;
    } else if (addr[0] == 0x33 && addr[1] == 0x33) {
        ESP_LOGW(TAG, "IPv6 multicast is always filtered in by W5500.");
    } else {
        ESP_LOGE(TAG, "W5500 filters in IP multicast frames only!");
        ret = ESP_ERR_NOT_SUPPORTED;
    }
err:
    return ret;
}

static esp_err_t emac_w5500_del_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);

    ESP_GOTO_ON_FALSE(!(addr[0] == 0x33 && addr[1] == 0x33), ESP_FAIL, err, TAG, "IPv6 multicast is always filtered in by W5500.");

    if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e && emac->mcast_cnt > 0) {
        emac->mcast_cnt--;
    }
    if (emac->mcast_cnt == 0) {
        // W5500 doesn't have specific MAC filter, so we just block multicast
        ESP_GOTO_ON_ERROR(emac_w5500_set_block_ip4_mcast(mac, true), err, TAG, "set block multicast failed");
    }
err:
    return ret;
}

static esp_err_t emac_w5500_set_link(esp_eth_mac_t *mac, eth_link_t link)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    switch (link) {
    case ETH_LINK_UP:
        ESP_LOGD(TAG, "link is up");
        ESP_GOTO_ON_ERROR(mac->start(mac), err, TAG, "w5500 start failed");
        if (emac->poll_timer) {
            ESP_GOTO_ON_ERROR(esp_timer_start_periodic(emac->poll_timer, emac->poll_period_ms * 1000),
                                err, TAG, "start poll timer failed");
        }
        break;
    case ETH_LINK_DOWN:
        ESP_LOGD(TAG, "link is down");
        ESP_GOTO_ON_ERROR(mac->stop(mac), err, TAG, "w5500 stop failed");
        if (emac->poll_timer) {
            ESP_GOTO_ON_ERROR(esp_timer_stop(emac->poll_timer),
                                err, TAG, "stop poll timer failed");
        }
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown link status");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w5500_set_speed(esp_eth_mac_t *mac, eth_speed_t speed)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    switch (speed) {
    case ETH_SPEED_10M:
        emac->tx_tmo = W5500_10M_TX_TMO_US;
        ESP_LOGD(TAG, "working in 10Mbps");
        break;
    case ETH_SPEED_100M:
        emac->tx_tmo = W5500_100M_TX_TMO_US;
        ESP_LOGD(TAG, "working in 100Mbps");
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown speed");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w5500_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex)
{
    esp_err_t ret = ESP_OK;
    switch (duplex) {
    case ETH_DUPLEX_HALF:
        ESP_LOGD(TAG, "working in half duplex");
        break;
    case ETH_DUPLEX_FULL:
        ESP_LOGD(TAG, "working in full duplex");
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown duplex");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w5500_set_promiscuous(esp_eth_mac_t *mac, bool enable)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    uint8_t smr = 0;
    ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_MR(0), &smr, sizeof(smr)), err, TAG, "read SMR failed");
    if (enable) {
        smr &= ~W5500_SMR_MAC_FILTER;
    } else {
        smr |= W5500_SMR_MAC_FILTER;
    }
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_MR(0), &smr, sizeof(smr)), err, TAG, "write SMR failed");

err:
    return ret;
}

static esp_err_t emac_w5500_set_all_multicast(esp_eth_mac_t *mac, bool enable)
{
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    ESP_RETURN_ON_ERROR(emac_w5500_set_block_ip4_mcast(mac, !enable), TAG, "set block multicast failed");
    emac->mcast_cnt = 0;
    if (enable) {
        ESP_LOGW(TAG, "W5500 filters in IP multicast frames only!");
    } else {
        ESP_LOGW(TAG, "W5500 always filters in IPv6 multicast frames!");
    }
    return ESP_OK;
}

static esp_err_t emac_w5500_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable)
{
    /* w5500 doesn't support flow control function, so accept any value */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t emac_w5500_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability)
{
    /* w5500 doesn't support PAUSE function, so accept any value */
    return ESP_ERR_NOT_SUPPORTED;
}

static inline bool is_w5500_sane_for_rxtx(emac_w5500_t *emac)
{
    uint8_t phycfg;
    /* phy is ok for rx and tx operations if bits RST and LNK are set (no link down, no reset) */
    if (w5500_read(emac, W5500_REG_PHYCFGR, &phycfg, 1) == ESP_OK && (phycfg & 0x8001)) {
        return true;
    }
   return false;
}

// Pushes one frame into the W5500 TX buffer and issues SEND.  Does NOT wait
// for SIR_SEND — caller signals that by holding tx_idle_sem; the task releases
// the sem when the ISR-driven SIR_SEND wakes it.  Caller MUST already hold
// tx_idle_sem before calling.
static esp_err_t w5500_start_tx(emac_w5500_t *emac, const uint8_t *buf, uint32_t length)
{
    uint16_t free_size = 0;
    esp_err_t err;
    if ((err = w5500_get_tx_free_size(emac, &free_size)) != ESP_OK) return err;
    if (length > free_size) {
        ESP_LOGW(TAG, "TX stuck (free=%" PRIu16 " need=%" PRIu32 "), recovering socket", free_size, length);
        if ((err = w5500_reopen_socket(emac)) != ESP_OK) return err;
        if ((err = w5500_get_tx_free_size(emac, &free_size)) != ESP_OK) return err;
        if (length > free_size) return ESP_FAIL;
    }
    uint16_t offset = 0;
    if ((err = w5500_read(emac, W5500_REG_SOCK_TX_WR(0), &offset, sizeof(offset))) != ESP_OK) return err;
    offset = __builtin_bswap16(offset);
    if ((err = w5500_write_buffer(emac, buf, length, offset)) != ESP_OK) return err;
    offset += length;
    offset = __builtin_bswap16(offset);
    if ((err = w5500_write(emac, W5500_REG_SOCK_TX_WR(0), &offset, sizeof(offset))) != ESP_OK) return err;
    // Original way: issue command and read it back to make sure it's accepted by W5500. However, this readback costs a full SPI round-trip 
    // and is not necessary since tx_idle_sem already guarantees that the previous SEND command has completed (and thus the W5500 is ready for the next one).
    //if ((err = w5500_send_command(emac, W5500_SCR_SEND, 100)) != ESP_OK) return err;

    // Just issue the command without waiting for it to take effect.
    if ((err = w5500_issue_send(emac)) != ESP_OK) return err;
    return ESP_OK;
}

static esp_err_t emac_w5500_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length)
{
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);

    ESP_RETURN_ON_FALSE(length <= ETH_MAX_PACKET_SIZE, ESP_ERR_INVALID_ARG, TAG,
                        "frame size is too big (actual %" PRIu32 ", maximum %u)", length, ETH_MAX_PACKET_SIZE);

    // Fast path: short, DMA-capable, 4-byte-aligned frames go straight to the
    // W5500 with no memcpy and no task handoff when the TX engine is idle AND
    // nothing is queued ahead of us (preserves frame order wrt the queued path).
    // For > W5500_SPI_POLLING_THRESHOLD bytes the SPI write itself uses the
    // interrupt-driven DMA path, so the lwIP thread sleeps during the transfer
    // instead of busy-waiting.  tx_idle_sem stays taken until the SIR_SEND ISR
    // wakes the emac task, which releases it — lwIP returns before completion.
    if (length <= W5500_TX_INLINE_THRESHOLD &&
            esp_ptr_dma_capable(buf) &&
            (((uintptr_t)buf & 0x3) == 0) &&
            xSemaphoreTake(emac->tx_idle_sem, 0) == pdTRUE) {
        if (uxQueueMessagesWaiting(emac->tx_queue) == 0) {
            if (w5500_start_tx(emac, buf, length) == ESP_OK) {
                emac->tx_start_us = esp_timer_get_time();
                // tx_active_buf stays NULL — no pool buffer to return on completion.
                return ESP_OK;
            }
        }
        xSemaphoreGive(emac->tx_idle_sem);
        // fall through to queued path
    }

    // Queued path: grab a free DMA buffer from the pool (wait briefly to absorb
    // micro-bursts), copy the frame, and let the emac task drive the SPI write.
    uint8_t *frame_buf = NULL;
    if (xQueueReceive(emac->tx_pool, &frame_buf, pdMS_TO_TICKS(2)) != pdTRUE) {
        ESP_LOGD(TAG, "TX pool exhausted — dropping frame");
        return ESP_FAIL;
    }

    memcpy(frame_buf, buf, length);
    w5500_tx_frame_t frame = { .data = frame_buf, .len = length };
    xQueueSend(emac->tx_queue, &frame, 0);      // pool and queue are same depth — space is guaranteed
    xTaskNotifyGive(emac->rx_task_hdl);         // wake emac_w5500_task to drain the queue
    return ESP_OK;
}

static esp_err_t emac_w5500_alloc_recv_buf(emac_w5500_t *emac, uint8_t **buf, uint32_t *length)
{
    esp_err_t ret = ESP_OK;
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint32_t copy_len = 0;
    uint16_t remain_bytes = 0;
    *buf = NULL;

    w5500_get_rx_received_size(emac, &remain_bytes);
    if (remain_bytes) {
        // get current read pointer
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_RX_RD(0), &offset, sizeof(offset)), err, TAG, "read RX RD failed");
        offset = __builtin_bswap16(offset);
        // read head
        ESP_GOTO_ON_ERROR(w5500_read_buffer(emac, &rx_len, sizeof(rx_len), offset), err, TAG, "read frame header failed");
        rx_len = __builtin_bswap16(rx_len) - 2; // data size includes 2 bytes of header
        // frames larger than expected will be truncated
        copy_len = rx_len > *length ? *length : rx_len;
        // runt frames are not forwarded by W5500 (tested on target), but check the length anyway since it could be corrupted at SPI bus
        ESP_GOTO_ON_FALSE(copy_len >= ETH_MIN_PACKET_SIZE - ETH_CRC_LEN, ESP_ERR_INVALID_SIZE, err, TAG, "invalid frame length %" PRIu32, copy_len);
        *buf = malloc(copy_len);
        if (*buf != NULL) {
            emac_w5500_auto_buf_info_t *buff_info = (emac_w5500_auto_buf_info_t *)*buf;
            buff_info->offset = offset;
            buff_info->copy_len = copy_len;
            buff_info->rx_len = rx_len;
            buff_info->remain = remain_bytes;
        } else {
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
    }
err:
    *length = rx_len;
    return ret;
}

static esp_err_t emac_w5500_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint16_t copy_len = 0;
    uint16_t remain_bytes = 0;
    emac->packets_remain = false;

    if (*length != W5500_ETH_MAC_RX_BUF_SIZE_AUTO) {
        w5500_get_rx_received_size(emac, &remain_bytes);
        if (remain_bytes) {
            // get current read pointer
            ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_RX_RD(0), &offset, sizeof(offset)), err, TAG, "read RX RD failed");
            offset = __builtin_bswap16(offset);
            // read head first
            ESP_GOTO_ON_ERROR(w5500_read_buffer(emac, &rx_len, sizeof(rx_len), offset), err, TAG, "read frame header failed");
            rx_len = __builtin_bswap16(rx_len) - 2; // data size includes 2 bytes of header
            // frames larger than expected will be truncated
            copy_len = rx_len > *length ? *length : rx_len;
        } else {
            // silently return when no frame is waiting
            goto err;
        }
    } else {
        emac_w5500_auto_buf_info_t *buff_info = (emac_w5500_auto_buf_info_t *)buf;
        offset = buff_info->offset;
        copy_len = buff_info->copy_len;
        rx_len = buff_info->rx_len;
        remain_bytes = buff_info->remain;
    }
    // 2 bytes of header
    offset += 2;
    // read the payload
    ESP_GOTO_ON_ERROR(w5500_read_buffer(emac, emac->rx_buffer, copy_len, offset), err, TAG, "read payload failed, len=%" PRIu16 ", offset=%" PRIu16, rx_len, offset);
    memcpy(buf, emac->rx_buffer, copy_len);
    offset += rx_len;
    // update read pointer
    offset = __builtin_bswap16(offset);
    ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_RX_RD(0), &offset, sizeof(offset)), err, TAG, "write RX RD failed");
    /* issue RECV command */
    ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_RECV, 100), err, TAG, "issue RECV command failed");
    // check if there're more data need to process
    remain_bytes -= rx_len + 2;
    emac->packets_remain = remain_bytes > 0;

    *length = copy_len;
    return ret;
err:
    *length = 0;
    return ret;
}

static esp_err_t emac_w5500_flush_recv_frame(emac_w5500_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint16_t remain_bytes = 0;
    emac->packets_remain = false;

    w5500_get_rx_received_size(emac, &remain_bytes);
    if (remain_bytes) {
        // get current read pointer
        ESP_GOTO_ON_ERROR(w5500_read(emac, W5500_REG_SOCK_RX_RD(0), &offset, sizeof(offset)), err, TAG, "read RX RD failed");
        offset = __builtin_bswap16(offset);
        // read head first
        ESP_GOTO_ON_ERROR(w5500_read_buffer(emac, &rx_len, sizeof(rx_len), offset), err, TAG, "read frame header failed");
        // update read pointer
        rx_len = __builtin_bswap16(rx_len);
        offset += rx_len;
        offset = __builtin_bswap16(offset);
        ESP_GOTO_ON_ERROR(w5500_write(emac, W5500_REG_SOCK_RX_RD(0), &offset, sizeof(offset)), err, TAG, "write RX RD failed");
        /* issue RECV command */
        ESP_GOTO_ON_ERROR(w5500_send_command(emac, W5500_SCR_RECV, 100), err, TAG, "issue RECV command failed");
        // check if there're more data need to process
        remain_bytes -= rx_len;
        emac->packets_remain = remain_bytes > 0;
    }
err:
    return ret;
}

IRAM_ATTR static void w5500_isr_handler(void *arg)
{
    emac_w5500_t *emac = (emac_w5500_t *)arg;
    BaseType_t high_task_wakeup = pdFALSE;
    /* notify w5500 task */
    vTaskNotifyGiveFromISR(emac->rx_task_hdl, &high_task_wakeup);
    if (high_task_wakeup != pdFALSE) {
        portYIELD_FROM_ISR();
    }
}

static void w5500_poll_timer(void *arg)
{
    emac_w5500_t *emac = (emac_w5500_t *)arg;
    xTaskNotifyGive(emac->rx_task_hdl);
}

static void emac_w5500_task(void *arg)
{
    emac_w5500_t *emac = (emac_w5500_t *)arg;
    uint8_t status = 0;
    uint8_t *buffer = NULL;
    uint32_t frame_len = 0;
    uint32_t buf_len = 0;
    esp_err_t ret;
    while (1) {
        /* Wait for a notification: either the W5500 INT pin fired (RX ready)
         * or emac_w5500_transmit() enqueued a TX frame. */
        bool do_hw_check;
        if (emac->int_gpio_num >= 0) {
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            /* Check hardware if notified OR if INT is still asserted (missed-edge safety net) */
            do_hw_check = (notified > 0) || (gpio_get_level(emac->int_gpio_num) == 0);
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            do_hw_check = true;
        }

        if (do_hw_check) {
            /* Detect W5500 hardware reset (power glitch): socket mode reverts to
             * 0x00 (TCP default) and buffer sizes shrink to 2KB. */
            uint8_t sock_mr = 0;
            if (w5500_read(emac, W5500_REG_SOCK_MR(0), &sock_mr, sizeof(sock_mr)) == ESP_OK
                    && sock_mr != (W5500_SMR_MAC_RAW | W5500_SMR_MAC_FILTER | W5500_SMR_MAC_BLOCK_MCAST)) {
                ESP_LOGW(TAG, "W5500 register reset detected (SOCK_MR=0x%02X) — re-initialising", sock_mr);
                w5500_reopen_socket(emac);
            }

            /* Read SOCK_IR once and dispatch both TX and RX events from it.
             * SIR_SEND and SIR_RECV can be set simultaneously; handling them in
             * one read avoids a second SPI transaction and prevents missed edges. */
            w5500_read(emac, W5500_REG_SOCK_IR(0), &status, sizeof(status));

            /* TX done: INT fired for SIR_SEND, or SIR_SEND was already set when
             * we woke for SIR_RECV — caught either way in this single read.
             * tx_active_buf is non-NULL only for the queued path; the inline
             * path owns lwIP-provided memory, so there's no pool buffer to
             * return there. */
            if (status & W5500_SIR_SEND) {
                uint8_t clr = W5500_SIR_SEND;
                w5500_write(emac, W5500_REG_SOCK_IR(0), &clr, sizeof(clr));
                if (emac->tx_active_buf) {
                    xQueueSend(emac->tx_pool, &emac->tx_active_buf, 0);
                    emac->tx_active_buf = NULL;
                }
                xSemaphoreGive(emac->tx_idle_sem);
            }

            /* RX: drain all received frames from the W5500 RX buffer */
            if (status & W5500_SIR_RECV) {
                uint8_t clr = W5500_SIR_RECV;
                w5500_write(emac, W5500_REG_SOCK_IR(0), &clr, sizeof(clr));
                do {
                    frame_len = ETH_MAX_PACKET_SIZE;
                    if ((ret = emac_w5500_alloc_recv_buf(emac, &buffer, &frame_len)) == ESP_OK) {
                        if (buffer != NULL) {
                            buf_len = W5500_ETH_MAC_RX_BUF_SIZE_AUTO;
                            if (emac->parent.receive(&emac->parent, buffer, &buf_len) == ESP_OK) {
                                if (buf_len == 0) {
                                    free(buffer);
                                } else if (frame_len > buf_len) {
                                    ESP_LOGE(TAG, "received frame was truncated");
                                    free(buffer);
                                } else {
                                    ESP_LOGD(TAG, "receive len=%" PRIu32, buf_len);
                                    emac->eth->stack_input(emac->eth, buffer, buf_len);
                                }
                            } else {
                                ESP_LOGE(TAG, "frame read from module failed");
                                free(buffer);
                            }
                        } else if (frame_len) {
                            ESP_LOGE(TAG, "invalid combination of frame_len(%" PRIu32 ") and buffer pointer(%p)", frame_len, buffer);
                        }
                    } else if (ret == ESP_ERR_NO_MEM) {
                        ESP_LOGE(TAG, "no mem for receive buffer");
                        emac_w5500_flush_recv_frame(emac);
                    } else {
                        ESP_LOGE(TAG, "unexpected error 0x%x", ret);
                    }
                } while (emac->packets_remain);
            }
        }

        /* TX timeout: SIR_SEND never arrived — W5500 may have been power-reset.
         * Checked every wakeup so a 1 s notification timeout doesn't mask it.
         * "In flight" now means tx_idle_sem is taken (either the inline path
         * from lwIP or the queued path below is waiting for SIR_SEND). */
        if (uxSemaphoreGetCount(emac->tx_idle_sem) == 0 &&
                (esp_timer_get_time() - emac->tx_start_us) > (W5500_TX_DONE_TMO_MS * 1000ULL)) {
            ESP_LOGW(TAG, "TX DONE timeout — re-initialising W5500 socket");
            w5500_reopen_socket(emac);
            if (emac->tx_active_buf) {
                xQueueSend(emac->tx_pool, &emac->tx_active_buf, 0);
                emac->tx_active_buf = NULL;
            }
            xSemaphoreGive(emac->tx_idle_sem);
        }

        /* Start next queued TX when the engine is idle.  Writing the frame and
         * issuing SEND is non-blocking; SIR_SEND will be caught on the next
         * wakeup triggered by the W5500 INT pin. */
        if (xSemaphoreTake(emac->tx_idle_sem, 0) == pdTRUE) {
            w5500_tx_frame_t tx_frame;
            if (xQueueReceive(emac->tx_queue, &tx_frame, 0) == pdTRUE) {
                if (w5500_start_tx(emac, tx_frame.data, tx_frame.len) == ESP_OK) {
                    emac->tx_active_buf = tx_frame.data;
                    emac->tx_start_us   = esp_timer_get_time();
                    // tx_idle_sem stays taken until SIR_SEND ISR wakes us
                } else {
                    xQueueSend(emac->tx_pool, &tx_frame.data, 0);  // drop frame, return buffer
                    xSemaphoreGive(emac->tx_idle_sem);
                }
            } else {
                xSemaphoreGive(emac->tx_idle_sem);  // nothing to do
            }
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t emac_w5500_init(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    esp_eth_mediator_t *eth = emac->eth;
    if (emac->int_gpio_num >= 0) {
        gpio_func_sel(emac->int_gpio_num, PIN_FUNC_GPIO);
        gpio_input_enable(emac->int_gpio_num);
        gpio_pullup_en(emac->int_gpio_num);
        gpio_set_intr_type(emac->int_gpio_num, GPIO_INTR_NEGEDGE); // active low
        gpio_intr_enable(emac->int_gpio_num);
        gpio_isr_handler_add(emac->int_gpio_num, w5500_isr_handler, emac);
    }
    ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LLINIT, NULL), err, TAG, "lowlevel init failed");
    /* reset w5500 */
    ESP_GOTO_ON_ERROR(w5500_reset(emac), err, TAG, "reset w5500 failed");
    /* verify chip id */
    ESP_GOTO_ON_ERROR(w5500_verify_id(emac), err, TAG, "verify chip ID failed");
    /* default setup of internal registers */
    ESP_GOTO_ON_ERROR(w5500_setup_default(emac), err, TAG, "w5500 default setup failed");
    return ESP_OK;
err:
    if (emac->int_gpio_num >= 0) {
        gpio_isr_handler_remove(emac->int_gpio_num);
        gpio_reset_pin(emac->int_gpio_num);
    }
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    return ret;
}

static esp_err_t emac_w5500_deinit(esp_eth_mac_t *mac)
{
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    esp_eth_mediator_t *eth = emac->eth;
    mac->stop(mac);
    if (emac->int_gpio_num >= 0) {
        gpio_isr_handler_remove(emac->int_gpio_num);
        gpio_reset_pin(emac->int_gpio_num);
    }
    if (emac->poll_timer && esp_timer_is_active(emac->poll_timer)) {
        esp_timer_stop(emac->poll_timer);
    }
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    return ESP_OK;
}

static esp_err_t emac_w5500_del(esp_eth_mac_t *mac)
{
    emac_w5500_t *emac = __containerof(mac, emac_w5500_t, parent);
    if (emac == s_emac) s_emac = NULL;
    if (emac->poll_timer) {
        esp_timer_delete(emac->poll_timer);
    }
    vTaskDelete(emac->rx_task_hdl);
    if (emac->tx_queue) {
        vQueueDelete(emac->tx_queue);
    }
    if (emac->tx_pool) {
        vQueueDelete(emac->tx_pool);
    }
    if (emac->tx_idle_sem) {
        vSemaphoreDelete(emac->tx_idle_sem);
    }
    for (int i = 0; i < W5500_TX_QUEUE_DEPTH; i++) {
        heap_caps_free(emac->tx_pool_bufs[i]);  // safe with NULL
    }
    emac->spi.deinit(emac->spi.ctx);
    heap_caps_free(emac->rx_buffer);
    free(emac);
    return ESP_OK;
}

esp_err_t w5500_get_diag(w5500_diag_t *out)
{
    if (!s_emac || !out) return ESP_ERR_INVALID_STATE;
    memset(out, 0, sizeof(*out));

    uint16_t tmp16;

    /* Common registers */
    w5500_read(s_emac, W5500_REG_VERSIONR,  &out->version,  1);
    w5500_read(s_emac, W5500_REG_PHYCFGR,   &out->phycfgr,  1);
    w5500_read(s_emac, W5500_REG_SIMR,      &out->simr,     1);

    /* Socket 0 control/status */
    w5500_read(s_emac, W5500_REG_SOCK_MR(0),  &out->sock_mr,  1);
    w5500_read(s_emac, W5500_REG_SOCK_SR(0),  &out->sock_sr,  1);
    w5500_read(s_emac, W5500_REG_SOCK_IR(0),  &out->sock_ir,  1);
    w5500_read(s_emac, W5500_REG_SOCK_IMR(0), &out->sock_imr, 1);

    /* TX buffer pointers (16-bit, big-endian on wire) */
    w5500_read(s_emac, W5500_REG_SOCK_TX_FSR(0), &tmp16, 2); out->tx_fsr = __builtin_bswap16(tmp16);
    w5500_read(s_emac, W5500_REG_SOCK_TX_RD(0),  &tmp16, 2); out->tx_rd  = __builtin_bswap16(tmp16);
    w5500_read(s_emac, W5500_REG_SOCK_TX_WR(0),  &tmp16, 2); out->tx_wr  = __builtin_bswap16(tmp16);

    /* RX buffer pointers */
    w5500_read(s_emac, W5500_REG_SOCK_RX_RSR(0), &tmp16, 2); out->rx_rsr = __builtin_bswap16(tmp16);
    w5500_read(s_emac, W5500_REG_SOCK_RX_RD(0),  &tmp16, 2); out->rx_rd  = __builtin_bswap16(tmp16);
    w5500_read(s_emac, W5500_REG_SOCK_RX_WR(0),  &tmp16, 2); out->rx_wr  = __builtin_bswap16(tmp16);

    return ESP_OK;
}

esp_err_t w5500_reset_socket(void)
{
    if (!s_emac) return ESP_ERR_INVALID_STATE;
    return w5500_reopen_socket(s_emac);
}

esp_eth_mac_t *esp_eth_mac_new_w5500(const eth_w5500_config_t *w5500_config, const eth_mac_config_t *mac_config)
{
    esp_eth_mac_t *ret = NULL;
    emac_w5500_t *emac = NULL;
    ESP_GOTO_ON_FALSE(w5500_config && mac_config, NULL, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE((w5500_config->int_gpio_num >= 0) != (w5500_config->poll_period_ms > 0), NULL, err, TAG, "invalid configuration argument combination");
    emac = calloc(1, sizeof(emac_w5500_t));
    ESP_GOTO_ON_FALSE(emac, NULL, err, TAG, "no mem for MAC instance");
    /* bind methods and attributes */
    emac->sw_reset_timeout_ms = mac_config->sw_reset_timeout_ms;
    emac->int_gpio_num = w5500_config->int_gpio_num;
    emac->poll_period_ms = w5500_config->poll_period_ms;
    emac->parent.set_mediator = emac_w5500_set_mediator;
    emac->parent.init = emac_w5500_init;
    emac->parent.deinit = emac_w5500_deinit;
    emac->parent.start = emac_w5500_start;
    emac->parent.stop = emac_w5500_stop;
    emac->parent.del = emac_w5500_del;
    emac->parent.write_phy_reg = emac_w5500_write_phy_reg;
    emac->parent.read_phy_reg = emac_w5500_read_phy_reg;
    emac->parent.set_addr = emac_w5500_set_addr;
    emac->parent.get_addr = emac_w5500_get_addr;
    emac->parent.add_mac_filter = emac_w5500_add_mac_filter;
    emac->parent.rm_mac_filter = emac_w5500_del_mac_filter;
    emac->parent.set_speed = emac_w5500_set_speed;
    emac->parent.set_duplex = emac_w5500_set_duplex;
    emac->parent.set_link = emac_w5500_set_link;
    emac->parent.set_promiscuous = emac_w5500_set_promiscuous;
    emac->parent.set_all_multicast = emac_w5500_set_all_multicast;
    emac->parent.set_peer_pause_ability = emac_w5500_set_peer_pause_ability;
    emac->parent.enable_flow_ctrl = emac_w5500_enable_flow_ctrl;
    emac->parent.transmit = emac_w5500_transmit;
    emac->parent.receive = emac_w5500_receive;

    if (w5500_config->custom_spi_driver.init != NULL && w5500_config->custom_spi_driver.deinit != NULL
        && w5500_config->custom_spi_driver.read != NULL && w5500_config->custom_spi_driver.write != NULL) {
        ESP_LOGD(TAG, "Using user's custom SPI Driver");
        emac->spi.init = w5500_config->custom_spi_driver.init;
        emac->spi.deinit = w5500_config->custom_spi_driver.deinit;
        emac->spi.read = w5500_config->custom_spi_driver.read;
        emac->spi.write = w5500_config->custom_spi_driver.write;
        /* Custom SPI driver device init */
        ESP_GOTO_ON_FALSE((emac->spi.ctx = emac->spi.init(w5500_config->custom_spi_driver.config)) != NULL, NULL, err, TAG, "SPI initialization failed");
    } else {
        ESP_LOGD(TAG, "Using default SPI Driver");
        emac->spi.init = w5500_spi_init;
        emac->spi.deinit = w5500_spi_deinit;
        emac->spi.read = w5500_spi_read;
        emac->spi.write = w5500_spi_write;
        /* SPI device init */
        ESP_GOTO_ON_FALSE((emac->spi.ctx = emac->spi.init(w5500_config)) != NULL, NULL, err, TAG, "SPI initialization failed");
    }

    /* create w5500 task */
    BaseType_t core_num = tskNO_AFFINITY;
    if (mac_config->flags & ETH_MAC_FLAG_PIN_TO_CORE) {
        core_num = esp_cpu_get_core_id();
    }
    BaseType_t xReturned = xTaskCreatePinnedToCore(emac_w5500_task, "w5500_tsk", mac_config->rx_task_stack_size, emac,
                           mac_config->rx_task_prio, &emac->rx_task_hdl, core_num);
    ESP_GOTO_ON_FALSE(xReturned == pdPASS, NULL, err, TAG, "create w5500 task failed");

    // TX frame buffer pool: W5500_TX_QUEUE_DEPTH DMA-capable buffers pre-allocated.
    emac->tx_pool = xQueueCreate(W5500_TX_QUEUE_DEPTH, sizeof(uint8_t *));
    ESP_GOTO_ON_FALSE(emac->tx_pool, NULL, err, TAG, "create TX pool queue failed");
    for (int i = 0; i < W5500_TX_QUEUE_DEPTH; i++) {
        emac->tx_pool_bufs[i] = heap_caps_malloc(ETH_MAX_PACKET_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_GOTO_ON_FALSE(emac->tx_pool_bufs[i], NULL, err, TAG, "allocate TX pool buffer %d failed", i);
        xQueueSend(emac->tx_pool, &emac->tx_pool_bufs[i], 0);
    }
    emac->tx_queue = xQueueCreate(W5500_TX_QUEUE_DEPTH, sizeof(w5500_tx_frame_t));
    ESP_GOTO_ON_FALSE(emac->tx_queue, NULL, err, TAG, "create TX queue failed");

    // Binary semaphore with initial count = 1 (TX engine idle at boot).
    emac->tx_idle_sem = xSemaphoreCreateCounting(1, 1);
    ESP_GOTO_ON_FALSE(emac->tx_idle_sem, NULL, err, TAG, "create TX idle sem failed");

    emac->rx_buffer = heap_caps_malloc(ETH_MAX_PACKET_SIZE, MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(emac->rx_buffer, NULL, err, TAG, "RX buffer allocation failed");

    if (emac->int_gpio_num < 0) {
        const esp_timer_create_args_t poll_timer_args = {
            .callback = w5500_poll_timer,
            .name = "emac_spi_poll_timer",
            .arg = emac,
            .skip_unhandled_events = true
        };
        ESP_GOTO_ON_FALSE(esp_timer_create(&poll_timer_args, &emac->poll_timer) == ESP_OK, NULL, err, TAG, "create poll timer failed");
    }

    s_emac = emac;
    return &(emac->parent);

err:
    if (emac) {
        if (emac->poll_timer) {
            esp_timer_delete(emac->poll_timer);
        }
        if (emac->rx_task_hdl) {
            vTaskDelete(emac->rx_task_hdl);
        }
        if (emac->tx_queue) {
            vQueueDelete(emac->tx_queue);
        }
        if (emac->tx_pool) {
            vQueueDelete(emac->tx_pool);
        }
        if (emac->tx_idle_sem) {
            vSemaphoreDelete(emac->tx_idle_sem);
        }
        for (int i = 0; i < W5500_TX_QUEUE_DEPTH; i++) {
            heap_caps_free(emac->tx_pool_bufs[i]);
        }
        if (emac->spi.ctx) {
            emac->spi.deinit(emac->spi.ctx);
        }
        heap_caps_free(emac->rx_buffer);
        free(emac);
    }
    return ret;
}
