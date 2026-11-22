// Board bring-up: panel power -> SPI bus -> the e-paper's two SSD1683 controllers.
//
// The 792x272 panel is wider than one SSD1683 drives (400 sources), so two run it in
// cascade: the slave the left half, the master the right half, mirrored. A command with
// bit 7 set goes to the slave (0x24 -> 0xA4, 0x44 -> 0xC4, ...). Each controller drives
// 396 columns of its 400-pixel RAM row; the 4 that aren't on the glass sit at the seam
// end of both, so a frame row of 99 bytes splits with one byte in common: bytes 0..49 to
// the slave, 49..98 to the master (the shared byte's off-glass half lands in each one's
// dead columns).
//
// Facts (pins, commands, their values) come from Elecrow's factory source for this board,
// Sam Perkins' ESPHome driver for it (MIT), GxEPD2's GDEY0579T93 driver and the SSD1683
// datasheet. The code is our own.

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

// Pin map, from Elecrow's factory source for this board.
#define PIN_EPD_SCLK  12
#define PIN_EPD_MOSI  11
#define PIN_EPD_CS    45
#define PIN_EPD_DC    46
#define PIN_EPD_RST   47
#define PIN_EPD_BUSY  48    // high while the controller works
#define PIN_EPD_PWR   7     // the panel's power switch, high = on
#define PIN_LED_PWR   41    // the red power LED: lit, since e-paper keeps its image unpowered

#define EPD_HOST      SPI2_HOST
#define EPD_PCLK_HZ   (10 * 1000 * 1000)    // the SSD1683 takes up to 20 MHz writes

#define HALF_ROW      50                     // bytes of a frame row each controller gets
#define HALF_BYTES    (HALF_ROW * EPD_H)

#define RESET_TIMEOUT_MS    1000
#define START_TIMEOUT_MS    100              // BUSY rises within this of a refresh command
#define FULL_TIMEOUT_MS     6000             // typical: 2.2 s
#define PARTIAL_TIMEOUT_MS  3000             // typical: 0.45 s

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_sent;             // given when a RAM write's DMA has finished
static uint8_t *s_half;                      // one controller's half of a frame (DMA-capable)

static esp_err_t cmd(uint8_t c, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, c, data, len);
}

// a command and its data bytes, returning from the caller on error
#define CMD(c, ...) do {                                                                   \
        static const uint8_t d_[] = {__VA_ARGS__};                                         \
        ESP_RETURN_ON_ERROR(cmd((c), d_, sizeof(d_)), TAG, "command 0x%02x", (c));         \
    } while (0)
#define CMD0(c) ESP_RETURN_ON_ERROR(cmd((c), NULL, 0), TAG, "command 0x%02x", (c))

static esp_err_t wait_idle(int timeout_ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    vTaskDelay(pdMS_TO_TICKS(1));            // BUSY rises a moment after the command
    while (gpio_get_level(PIN_EPD_BUSY)) {
        if (esp_timer_get_time() > until) {
            ESP_LOGE(TAG, "e-paper still busy after %d ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

// A refresh must be seen to start: BUSY rises within a moment of the activation
// command. If it never does, the command was lost and the glass didn't change, so say
// so rather than let the caller record a frame the panel isn't showing.
static esp_err_t wait_refresh(int timeout_ms)
{
    int64_t start_by = esp_timer_get_time() + START_TIMEOUT_MS * 1000;
    while (!gpio_get_level(PIN_EPD_BUSY)) {
        if (esp_timer_get_time() > start_by) {
            ESP_LOGE(TAG, "e-paper refresh never started (BUSY stayed low)");
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return wait_idle(timeout_ms);
}

static bool IRAM_ATTR ram_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woke);
    return woke == pdTRUE;
}

// One RAM (0x24: the image to show; 0x26: the one shown now, which a partial refresh
// compares against) on both controllers, from a whole frame.
static esp_err_t write_ram(uint8_t ram, const uint8_t *frame)
{
    // slave: the left half, x and y counting up
    CMD(0x91, 0x03);                         // data entry: x+, y+
    CMD(0xC4, 0x00, 0x31);                   // RAM x: bytes 0..49
    CMD(0xC5, 0x00, 0x00, 0x0F, 0x01);       // RAM y: 0..271
    CMD(0xCE, 0x00);
    CMD(0xCF, 0x00, 0x00);
    for (int y = 0; y < EPD_H; y++)
        memcpy(s_half + y * HALF_ROW, frame + y * EPD_ROW_BYTES, HALF_ROW);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_io, ram | 0x80, s_half, HALF_BYTES), TAG, "slave RAM");
    xSemaphoreTake(s_sent, portMAX_DELAY);   // the DMA reads s_half until this

    // master: the right half, mirrored, so x counts down
    CMD(0x11, 0x02);                         // data entry: x-, y+
    CMD(0x44, 0x31, 0x00);                   // RAM x: bytes 49..0
    CMD(0x45, 0x00, 0x00, 0x0F, 0x01);
    CMD(0x4E, 0x31);
    CMD(0x4F, 0x00, 0x00);
    for (int y = 0; y < EPD_H; y++)
        memcpy(s_half + y * HALF_ROW, frame + y * EPD_ROW_BYTES + (EPD_ROW_BYTES - HALF_ROW), HALF_ROW);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_io, ram, s_half, HALF_BYTES), TAG, "master RAM");
    xSemaphoreTake(s_sent, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t epd_show(const uint8_t *frame, const uint8_t *prev, epd_mode_t mode)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err;
    if (mode == EPD_FULL) {
        ESP_RETURN_ON_ERROR(write_ram(0x24, frame), TAG, "new image");
        ESP_RETURN_ON_ERROR(write_ram(0x26, frame), TAG, "old image");   // the next partial's baseline
        CMD(0x3C, 0x03);                     // border: follows the waveform's LUT3 (white)
        CMD(0x21, 0x40, 0x10);               // the old RAM reads as black: every pixel is driven; cascade
        CMD(0x1A, 0x64, 0x00);               // temperature 100 C: the OTP's fast full waveform
        CMD(0x22, 0xD7);                     // load that waveform, display, power the analog side down
        CMD0(0x20);
        err = wait_refresh(FULL_TIMEOUT_MS);
    } else {
        ESP_RETURN_ON_ERROR(write_ram(0x24, frame), TAG, "new image");
        ESP_RETURN_ON_ERROR(write_ram(0x26, prev), TAG, "old image");
        CMD(0x3C, 0x80);                     // border: held at VCOM, so it doesn't blink
        CMD(0x21, 0x00, 0x10);               // compare new with old: unchanged pixels aren't driven; cascade
        CMD(0x22, 0xFF);                     // sensor temperature, black/white waveform, display, power down
        CMD0(0x20);
        err = wait_refresh(PARTIAL_TIMEOUT_MS);
    }
    ESP_LOGD(TAG, "%s refresh: %d ms", mode == EPD_FULL ? "full" : "partial",
             (int)((esp_timer_get_time() - t0) / 1000));
    return err;
}

esp_err_t epd_reinit(void)
{
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(wait_idle(RESET_TIMEOUT_MS), TAG, "reset");
    CMD0(0x12);                              // software reset
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(wait_idle(RESET_TIMEOUT_MS), TAG, "software reset");
    CMD(0x18, 0x80);                         // temperature from the built-in sensor
    CMD(0x22, 0xB1);                         // read it
    CMD0(0x20);
    ESP_RETURN_ON_ERROR(wait_idle(RESET_TIMEOUT_MS), TAG, "temperature");
    return ESP_OK;
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
    s_half = heap_caps_malloc(HALF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_sent && s_half, ESP_ERR_NO_MEM, TAG, "buffers");

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_EPD_SCLK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HALF_BYTES,
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
    ESP_LOGI(TAG, "board up: %dx%d e-paper (2x SSD1683)", EPD_W, EPD_H);
    return ESP_OK;
}
