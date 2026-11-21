// Board bring-up: SPI bus -> ST7306 reflective LCD. There's no PMIC to talk to:
// the PWR key and the battery charger are handled in hardware.

#include "board.h"
#include "board_rlcd.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

// Pin map, from the board's schematic.
#define PIN_LCD_SCLK  11
#define PIN_LCD_MOSI  12
#define PIN_LCD_DC    5
#define PIN_LCD_CS    40
#define PIN_LCD_RST   41

#define LCD_HOST      SPI2_HOST
#define LCD_PCLK_HZ   (10 * 1000 * 1000)

// The visible 300x400 in the controller's RAM: a column address covers 12
// pixels, a row address 2 rows.
#define COL_FIRST     0x12
#define COL_LAST      0x2A
#define ROW_FIRST     0x00
#define ROW_LAST      0xC7

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_sent;           // given when a frame's DMA has finished

typedef struct {
    uint8_t cmd, len;
    uint8_t data[10];
    uint16_t delay_ms;
} lcd_cmd_t;

// Power-up sequence for this panel. The register values are the ones Waveshare's
// examples for this board use (Apache-2.0); Zephyr's board definition has the same.
static const lcd_cmd_t s_init[] = {
    {0xD6, 2, {0x17, 0x02}, 0},                                                  // NVM load control
    {0xD1, 1, {0x01}, 0},                                                        // booster on
    {0xC0, 2, {0x11, 0x04}, 0},                                                  // gate voltages
    {0xC1, 4, {0x69, 0x69, 0x69, 0x69}, 0},                                      // source voltages
    {0xC2, 4, {0x19, 0x19, 0x19, 0x19}, 0},
    {0xC4, 4, {0x4B, 0x4B, 0x4B, 0x4B}, 0},
    {0xC5, 4, {0x19, 0x19, 0x19, 0x19}, 0},
    {0xD8, 2, {0x80, 0xE9}, 0},                                                  // oscillator
    {0xB2, 1, {0x02}, 0},                                                        // frame rate
    {0xB3, 10, {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 0}, // high-power gate waveform
    {0xB4, 8, {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 0},              // low-power gate waveform
    {0x62, 3, {0x32, 0x03, 0x1F}, 0},
    {0xB7, 1, {0x13}, 0},                                                        // source equalize
    {0xB0, 1, {0x64}, 0},                                                        // 400 gate lines
    {0x11, 0, {0}, 200},                                                         // sleep out
    {0xC9, 1, {0x00}, 0},                                                        // source voltage select
    {0x36, 1, {0x48}, 0},                                                        // memory access order
    {0x3A, 1, {0x11}, 0},                                                        // data format
    {0xB9, 1, {0x20}, 0},                                                        // black and white
    {0xB8, 1, {0x29}, 0},                                                        // panel settings
    {0x21, 0, {0}, 0},                                                           // inverted: a set bit is white
    {0x35, 1, {0x00}, 0},                                                        // tearing-effect line on vblank
    {0xD0, 1, {0xFF}, 0},                                                        // auto power-down on
    {0x38, 0, {0}, 0},                                                           // high-power mode
    {0x29, 0, {0}, 0},                                                           // display on
};

static bool IRAM_ATTR frame_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woke);
    return woke == pdTRUE;
}

esp_err_t rlcd_send_frame(const uint8_t *frame)
{
    static const uint8_t cols[] = {COL_FIRST, COL_LAST}, rows[] = {ROW_FIRST, ROW_LAST};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, 0x2A, cols, 2), TAG, "columns");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, 0x2B, rows, 2), TAG, "rows");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_io, 0x2C, frame, RLCD_FRAME_BYTES), TAG, "frame");
    xSemaphoreTake(s_sent, portMAX_DELAY);     // the DMA reads `frame` until this
    return ESP_OK;
}

esp_err_t board_init(void)
{
    s_sent = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_sent, ESP_ERR_NO_MEM, TAG, "semaphore");

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RLCD_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = frame_sent,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_io),
                        TAG, "panel io");

    gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_LCD_RST, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset pin");
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        const lcd_cmd_t *c = &s_init[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len),
                            TAG, "init 0x%02x", c->cmd);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
    ESP_LOGI(TAG, "board up: %dx%d reflective LCD", RLCD_COLS, RLCD_ROWS);
    return ESP_OK;
}
