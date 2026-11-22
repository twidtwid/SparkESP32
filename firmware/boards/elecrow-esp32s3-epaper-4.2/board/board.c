// Board bring-up: panel power -> SPI bus -> the e-paper's controller.
//
// This is the V1.2 board (PCBA V1.2A, a green circular sticker on its back). Its panel
// controller takes UltraChip-style commands (UC8276C class); the earlier V1.0 board has an
// SSD1683, a different command set, and isn't supported by this port. The controller sleeps
// between refreshes, so each one starts from a hardware reset and the panel setup, sends the
// image shown now (0x10) and the one to show (0x13), loads a waveform into the LUT
// registers, refreshes (0x17), and puts the controller back to sleep:
//
//   - DU, "direct update": one short phase that only moves pixels that change: no flash.
//   - GC, "global clear": drives every pixel through black and white: flashes, but wipes
//     the ghosts DU leaves behind.
//
// Facts (pins, the setup and waveform values) come from Elecrow's V1.2 Arduino examples and
// ESPHome component for this board; the BUSY polarity was measured on the board. The code is
// our own.

#include "board.h"
#include "board_epd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

// Pin map, from Elecrow's source for this board (the same as its 5.79" sibling's).
#define PIN_EPD_SCLK  12
#define PIN_EPD_MOSI  11
#define PIN_EPD_CS    45
#define PIN_EPD_DC    46
#define PIN_EPD_RST   47
#define PIN_EPD_BUSY  48    // LOW while the controller works (an active-low BUSY_N), high when done
#define PIN_EPD_PWR   7     // the panel's power switch, high = on
#define PIN_LED_PWR   41    // the red power LED: lit, since e-paper keeps its image unpowered

#define EPD_HOST      SPI2_HOST
#define EPD_PCLK_HZ   (10 * 1000 * 1000)

#define REFRESH_TIMEOUT_MS  15000            // Elecrow's own limit for a full refresh
#define START_TIMEOUT_MS    100              // BUSY falls within this of the refresh command

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_sent;             // given when a RAM write's DMA has finished
static uint8_t *s_dma;                       // an image on its way out (internal, DMA-capable)

static esp_err_t cmd(uint8_t c, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, c, data, len);
}

#define CMD(c, ...) do {                                                                   \
        static const uint8_t d_[] = {__VA_ARGS__};                                         \
        ESP_RETURN_ON_ERROR(cmd((c), d_, sizeof(d_)), TAG, "command 0x%02x", (c));         \
    } while (0)

// Waveforms: one LUT a register, 0x20 (VCOM) then the four transitions 0x21 WW, 0x22 BW,
// 0x23 WB, 0x24 BB. Each is 42 bytes; only its first group (7 bytes) is used, the rest zero.
typedef uint8_t lut_group_t[7];
static const lut_group_t LUT_DU[5] = {
    {0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00},
    {0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00},
    {0x01, 0x94, 0x00, 0x00, 0x00, 0x01, 0x00},
    {0x01, 0x54, 0x00, 0x00, 0x00, 0x01, 0x00},
    {0x01, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00},
};
static const lut_group_t LUT_GC[5] = {
    {0x01, 0x14, 0x0A, 0x14, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
};

// Wait for a refresh to run. Measured on the board: BUSY falls at the refresh command and
// stays low until the waveform ends (about 1.2 s for GC), then rises. Elecrow's examples
// read it the other way round, and their ESPHome driver leans on a 15 s timeout. The
// refresh must be seen to start (BUSY low): if it never does, the command was lost and
// the glass didn't change, so say so rather than let the caller record a frame the panel
// isn't showing.
static esp_err_t wait_refresh(int timeout_ms)
{
    int64_t start_by = esp_timer_get_time() + START_TIMEOUT_MS * 1000;
    while (gpio_get_level(PIN_EPD_BUSY)) {
        if (esp_timer_get_time() > start_by) {
            ESP_LOGE(TAG, "e-paper refresh never started (BUSY stayed high)");
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!gpio_get_level(PIN_EPD_BUSY)) {
        if (esp_timer_get_time() > until) {
            ESP_LOGE(TAG, "e-paper still busy after %d ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static bool IRAM_ATTR ram_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woke);
    return woke == pdTRUE;
}

// The frames live in PSRAM; SPI DMA wants internal memory, so stage through s_dma.
static esp_err_t write_image(uint8_t ram, const uint8_t *frame)
{
    memcpy(s_dma, frame, EPD_FRAME_BYTES);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_io, ram, s_dma, EPD_FRAME_BYTES), TAG, "image 0x%02x", ram);
    xSemaphoreTake(s_sent, portMAX_DELAY);   // the DMA reads s_dma until this
    return ESP_OK;
}

static esp_err_t write_lut(const lut_group_t lut[5])
{
    uint8_t reg[42] = {0};
    for (int i = 0; i < 5; i++) {
        memcpy(reg, lut[i], sizeof(lut[i]));
        ESP_RETURN_ON_ERROR(cmd(0x20 + i, reg, sizeof(reg)), TAG, "LUT 0x%02x", 0x20 + i);
    }
    return ESP_OK;
}

esp_err_t epd_reinit(void)
{
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    CMD(0x00, 0x3F, 0x4D);                   // panel setting: LUTs from registers, 400x300
    CMD(0x01, 0x03, 0x10, 0x3F, 0x3F, 0x03); // power setting
    CMD(0x06, 0x96, 0x96, 0x29);             // booster soft start
    CMD(0x30, 0x09);                         // frame rate (PLL)
    CMD(0x61, 0x01, 0x90, 0x01, 0x2C);       // resolution: 400 x 300
    CMD(0x82, 0x05);                         // VCOM DC
    CMD(0x50, 0x97);                         // VCOM and data interval, border
    CMD(0x60, 0x22);                         // non-overlap timing
    CMD(0xE3, 0x88);                         // power saving
    return ESP_OK;
}

esp_err_t epd_show(const uint8_t *frame, const uint8_t *prev, epd_mode_t mode)
{
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(epd_reinit(), TAG, "wake");            // it slept since the last refresh
    if (mode == EPD_PARTIAL) CMD(0x50, 0xD7);                 // DU: Elecrow's interval and border
    ESP_RETURN_ON_ERROR(write_image(0x10, prev), TAG, "old image");
    ESP_RETURN_ON_ERROR(write_image(0x13, frame), TAG, "new image");
    ESP_RETURN_ON_ERROR(write_lut(mode == EPD_FULL ? LUT_GC : LUT_DU), TAG, "waveform");
    CMD(0x17, 0xA5);                         // power on, refresh, power off
    esp_err_t err = wait_refresh(REFRESH_TIMEOUT_MS);
    CMD(0x07, 0xA5);                         // deep sleep until the next reset
    ESP_LOGD(TAG, "%s refresh: %d ms", mode == EPD_FULL ? "full" : "partial",
             (int)((esp_timer_get_time() - t0) / 1000));
    return err;
}

esp_err_t board_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_PWR) | (1ULL << PIN_LED_PWR) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "output pins");
    gpio_config_t busy = {.pin_bit_mask = 1ULL << PIN_EPD_BUSY, .mode = GPIO_MODE_INPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "busy pin");
    gpio_set_level(PIN_EPD_RST, 1);
    gpio_set_level(PIN_LED_PWR, 1);
    gpio_set_level(PIN_EPD_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));           // the panel's supply settles

    s_sent = xSemaphoreCreateBinary();
    s_dma = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_sent && s_dma, ESP_ERR_NO_MEM, TAG, "buffers");

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_EPD_SCLK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = PIN_EPD_DC,
        .cs_gpio_num = PIN_EPD_CS,
        .pclk_hz = EPD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .on_color_trans_done = ram_sent,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPD_HOST, &io, &s_io),
                        TAG, "panel io");

    ESP_RETURN_ON_ERROR(epd_reinit(), TAG, "e-paper init");
    ESP_LOGI(TAG, "board up: %dx%d e-paper (V1.2 controller)", EPD_W, EPD_H);
    return ESP_OK;
}
