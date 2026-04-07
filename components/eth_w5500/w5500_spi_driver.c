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
        .read_spi_fail   = s_stats.read_spi_fail,
        .read_timeout    = s_stats.read_timeout,
        .write_spi_fail  = s_stats.write_spi_fail,
        .write_timeout   = s_stats.write_timeout,
        .write_zero_copy = s_stats.write_zero_copy,
        .write_bounce    = s_stats.write_bounce,
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
    assert(len <= ETH_MAX_PACKET_SIZE);

    // Small register writes (≤4 bytes): inline in the transaction struct, no DMA.
    // This mirrors the read path and avoids DMA setup for the frequent per-frame
    // control register writes (TX_WR pointer, SEND command, SOCK_IR clear).
    bool use_inline = (len <= 4);

    // Large writes: use caller's buffer directly if it's DMA-capable and 4-byte
    // aligned (zero-copy), otherwise bounce through tx_dma_buf.
    // lwIP pbufs on ESP32-C3 are in internal SRAM (DMA-capable, MEM_ALIGNMENT=4)
    // so this hits for nearly all forwarded frames.
    // Unlike RX, TX has no length-rounding requirement on GDMA AHB.
    bool direct_dma = !use_inline && esp_ptr_dma_capable(data) && (((uintptr_t)data) & 3) == 0;

    spi_transaction_t t = {
        .flags     = use_inline ? SPI_TRANS_USE_TXDATA : 0,
        .cmd       = cmd,
        .addr      = addr,
        .length    = 8 * len,
        .tx_buffer = (use_inline || direct_dma) ? NULL : ctx->tx_dma_buf,
    };
    if (use_inline) {
        memcpy(t.tx_data, data, len);   // copy before taking the lock
    }
    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(W5500_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
        if (!use_inline) {
            if (direct_dma) {
                t.tx_buffer = data;     // point at caller's buffer inside the lock
                s_stats.write_zero_copy++;
            } else {
                // tx_dma_buf is shared — copy inside the mutex to avoid races
                memcpy(ctx->tx_dma_buf, data, len);
                s_stats.write_bounce++;
            }
        }
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
