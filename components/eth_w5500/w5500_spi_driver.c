#include "sdkconfig.h"

#if defined(CONFIG_ETH_DOWNLINK_W5500)

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_spec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "w5500_spi_driver.h"

static const char *TAG = "w5500.spi";

#define W5500_SPI_LOCK_TIMEOUT_MS  50
#define W5500_TX_DMA_ALIGN         64   /* cache-line safe on all ESP32 variants */

typedef struct {
    spi_device_handle_t hdl;
    SemaphoreHandle_t   lock;
    uint8_t            *tx_dma_buf;
} w5500_spi_ctx_t;

static void *w5500_custom_spi_init(const void *cfg)
{
    const eth_w5500_config_t *w5500_cfg = (const eth_w5500_config_t *)cfg;
    w5500_spi_ctx_t *ctx = calloc(1, sizeof(w5500_spi_ctx_t));
    if (!ctx) return NULL;

    // SPI bus is already initialized by the caller; just add our device.
    if (spi_bus_add_device(w5500_cfg->spi_host_id, w5500_cfg->spi_devcfg, &ctx->hdl) != ESP_OK) {
        free(ctx); return NULL;
    }
    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) { spi_bus_remove_device(ctx->hdl); free(ctx); return NULL; }

    ctx->tx_dma_buf = heap_caps_aligned_alloc(W5500_TX_DMA_ALIGN, ETH_MAX_PACKET_SIZE,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ctx->tx_dma_buf) {
        vSemaphoreDelete(ctx->lock); spi_bus_remove_device(ctx->hdl); free(ctx); return NULL;
    }
    ESP_LOGI(TAG, "DMA TX buffer at %p", ctx->tx_dma_buf);
    return ctx;
}

static esp_err_t w5500_custom_spi_deinit(void *spi_ctx)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    heap_caps_free(ctx->tx_dma_buf);
    vSemaphoreDelete(ctx->lock);
    spi_bus_remove_device(ctx->hdl);
    free(ctx);
    return ESP_OK;
}

static esp_err_t w5500_custom_spi_read(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                        void *data, uint32_t len)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    // Small register reads (≤4 bytes) use SPI_TRANS_USE_RXDATA (inline, no DMA needed).
    // Larger reads land in emac->rx_buffer which is already MALLOC_CAP_DMA (allocated
    // by the driver in esp_eth_mac_new_w5500), so no bounce buffer is needed there either.
    spi_transaction_t t = {
        .flags     = len <= 4 ? SPI_TRANS_USE_RXDATA : 0,
        .cmd       = cmd,
        .addr      = addr,
        .length    = 8 * len,
        .rx_buffer = data,
    };
    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
        if (spi_device_polling_transmit(ctx->hdl, &t) == ESP_OK) ret = ESP_OK;
        xSemaphoreGive(ctx->lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    if ((t.flags & SPI_TRANS_USE_RXDATA) && len <= 4) memcpy(data, t.rx_data, len);
    return ret;
}

static esp_err_t w5500_custom_spi_write(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                         const void *data, uint32_t len)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    // Copy into the pre-allocated DMA-aligned buffer so setup_dma_priv_buffer()
    // in the SPI master driver sees an already-suitable pointer and skips the
    // per-frame heap allocation that was causing DMA heap exhaustion.
    assert(len <= ETH_MAX_PACKET_SIZE);
    spi_transaction_t t = {
        .cmd       = cmd,
        .addr      = addr,
        .length    = 8 * len,
        .tx_buffer = ctx->tx_dma_buf,
    };
    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
        // memcpy inside the mutex: tx_dma_buf is shared, must not race
        memcpy(ctx->tx_dma_buf, data, len);
        if (spi_device_polling_transmit(ctx->hdl, &t) == ESP_OK) ret = ESP_OK;
        xSemaphoreGive(ctx->lock);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

void w5500_spi_driver_config(eth_spi_custom_driver_config_t *driver,
                              const eth_w5500_config_t *w5500)
{
    driver->config = (void *)w5500;
    driver->init   = w5500_custom_spi_init;
    driver->deinit = w5500_custom_spi_deinit;
    driver->read   = w5500_custom_spi_read;
    driver->write  = w5500_custom_spi_write;
}

#endif // CONFIG_ETH_DOWNLINK_W5500
