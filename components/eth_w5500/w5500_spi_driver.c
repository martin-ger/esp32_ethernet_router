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

#include "esp_memory_utils.h"
#include "w5500_spi_driver.h"

static const char *TAG = "w5500.spi";

static volatile w5500_spi_stats_t s_stats;

w5500_spi_stats_t w5500_spi_get_stats(void)
{
    // Snapshot — counters are 32-bit and written atomically on RISC-V
    w5500_spi_stats_t snap = {
        .read_spi_fail  = s_stats.read_spi_fail,
        .read_timeout   = s_stats.read_timeout,
        .write_spi_fail = s_stats.write_spi_fail,
        .write_timeout  = s_stats.write_timeout,
    };
    return snap;
}

#define W5500_SPI_LOCK_TIMEOUT_MS  50
#define W5500_DMA_ALIGN            64   /* cache-line safe on all ESP32 variants */

// ESP32-C3 GDMA RX burst requires 4-byte aligned length
// (GDMA_LL_AHB_RX_BURST_NEEDS_ALIGNMENT).  Round up RX transfer lengths so
// setup_dma_priv_buffer() sees our buffer as suitable and skips heap allocation.
// Only applied to reads — writes must use exact len or W5500 auto-increments
// and corrupts adjacent registers.
#define DMA_RX_ROUND_UP(n)  (((n) + 3) & ~3)

// Buffer size: ETH_MAX_PACKET_SIZE rounded up to 4 bytes so rounded reads fit.
#define W5500_DMA_BUF_SIZE  DMA_RX_ROUND_UP(ETH_MAX_PACKET_SIZE)

typedef struct {
    spi_device_handle_t hdl;
    SemaphoreHandle_t   lock;
    uint8_t            *tx_dma_buf;
    uint8_t            *rx_dma_buf;
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

    ctx->tx_dma_buf = heap_caps_aligned_alloc(W5500_DMA_ALIGN, W5500_DMA_BUF_SIZE,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ctx->tx_dma_buf) {
        vSemaphoreDelete(ctx->lock); spi_bus_remove_device(ctx->hdl); free(ctx); return NULL;
    }
    ctx->rx_dma_buf = heap_caps_aligned_alloc(W5500_DMA_ALIGN, W5500_DMA_BUF_SIZE,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ctx->rx_dma_buf) {
        heap_caps_free(ctx->tx_dma_buf);
        vSemaphoreDelete(ctx->lock); spi_bus_remove_device(ctx->hdl); free(ctx); return NULL;
    }
    ESP_LOGI(TAG, "DMA TX=%p RX=%p buf_size=%u tx_dma_ok=%d rx_dma_ok=%d",
             ctx->tx_dma_buf, ctx->rx_dma_buf, W5500_DMA_BUF_SIZE,
             esp_ptr_dma_capable(ctx->tx_dma_buf),
             esp_ptr_dma_capable(ctx->rx_dma_buf));
    return ctx;
}

static esp_err_t w5500_custom_spi_deinit(void *spi_ctx)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    heap_caps_free(ctx->rx_dma_buf);
    heap_caps_free(ctx->tx_dma_buf);
    vSemaphoreDelete(ctx->lock);
    spi_bus_remove_device(ctx->hdl);
    free(ctx);
    return ESP_OK;
}

static IRAM_ATTR esp_err_t w5500_custom_spi_read(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                                  void *data, uint32_t len)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    // Small register reads (≤4 bytes): inline, no DMA.
    // Larger reads: if the caller's buffer is DMA-capable AND the length is
    // 4-byte aligned (GDMA RX burst requirement on C3), read directly into it
    // (zero-copy).  Otherwise bounce through rx_dma_buf.
    bool use_inline = (len <= 4);
    bool direct_dma = !use_inline && esp_ptr_dma_capable(data) && ((((uintptr_t)data) | len) & 3) == 0;
    uint32_t spi_len = use_inline ? len : DMA_RX_ROUND_UP(len);
    spi_transaction_t t = {
        .flags     = use_inline ? SPI_TRANS_USE_RXDATA : 0,
        .cmd       = cmd,
        .addr      = addr,
        .length    = 8 * spi_len,
        .rx_buffer = use_inline ? NULL : (direct_dma ? data : ctx->rx_dma_buf),
    };
    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
        if (spi_device_polling_transmit(ctx->hdl, &t) == ESP_OK) {
            if (use_inline) {
                memcpy(data, t.rx_data, len);
            } else if (!direct_dma) {
                memcpy(data, ctx->rx_dma_buf, len);
            }
            // direct_dma: data already in caller's buffer, no copy needed
            ret = ESP_OK;
        } else {
            s_stats.read_spi_fail++;
        }
        xSemaphoreGive(ctx->lock);
    } else {
        s_stats.read_timeout++;
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

static IRAM_ATTR esp_err_t w5500_custom_spi_write(void *spi_ctx, uint32_t cmd, uint32_t addr,
                                                   const void *data, uint32_t len)
{
    w5500_spi_ctx_t *ctx = spi_ctx;
    // Pre-allocated tx_dma_buf with DMA-aligned transfer length so
    // setup_dma_priv_buffer() skips the per-frame heap allocation.
    // rx_buffer stays NULL — no RX DMA setup occurs (MISO data discarded).
    assert(len <= ETH_MAX_PACKET_SIZE);
    uint32_t spi_len = len;
    spi_transaction_t t = {
        .flags     = 0,
        .cmd       = cmd,
        .addr      = addr,
        .length    = 8 * spi_len,
        .tx_buffer = ctx->tx_dma_buf,
    };
    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
        // memcpy inside the mutex: tx_dma_buf is shared, must not race
        memcpy(ctx->tx_dma_buf, data, len);
        if (spi_device_polling_transmit(ctx->hdl, &t) == ESP_OK) {
            ret = ESP_OK;
        } else {
            s_stats.write_spi_fail++;
        }
        xSemaphoreGive(ctx->lock);
    } else {
        s_stats.write_timeout++;
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
