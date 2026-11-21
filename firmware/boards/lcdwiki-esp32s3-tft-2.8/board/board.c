// Board bring-up: SPI bus -> ILI9341V panel, backlight off until the first frame
// is on the glass. The LCD's reset line is the ESP32-S3's own EN, so there is no
// reset pin here: the panel gets a software reset instead.

#include "board.h"
#include "board_lcd.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "board";

// Pin map, from LCDWiki's pin table for the ES3C28P (and what its factory test drives).
#define PIN_LCD_SCLK  12
#define PIN_LCD_MOSI  11
#define PIN_LCD_CS    10
#define PIN_LCD_DC    46
#define PIN_LCD_BL    45      // backlight, high = on

#define LCD_HOST      SPI2_HOST
#define LCD_PCLK_HZ   (40 * 1000 * 1000)
#define LCD_MAX_XFER  (320 * 40 * 2)   // one LVGL strip (display.c)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

// The panel maker's power, VCOM and gamma settings for this IPS panel. Sleep-out,
// pixel format, memory access order, inversion and display-on go through the
// esp_lcd_ili9341 API instead.
static const ili9341_lcd_init_cmd_t s_init[] = {
    {0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},                        // power control B
    {0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},                  // power-on sequence
    {0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3, 0},                        // driver timing A
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},            // power control A
    {0xF7, (uint8_t[]){0x20}, 1, 0},                                    // pump ratio
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},                              // driver timing B
    {0xC0, (uint8_t[]){0x13}, 1, 0},                                    // power control 1
    {0xC1, (uint8_t[]){0x13}, 1, 0},                                    // power control 2
    {0xC5, (uint8_t[]){0x22, 0x35}, 2, 0},                              // VCOM 1
    {0xC7, (uint8_t[]){0xBD}, 1, 0},                                    // VCOM 2
    {0xB6, (uint8_t[]){0x0A, 0xA2}, 2, 0},                              // display function
    {0xF6, (uint8_t[]){0x01, 0x30}, 2, 0},                              // interface
    {0xB1, (uint8_t[]){0x00, 0x1B}, 2, 0},                              // frame rate
    {0xF2, (uint8_t[]){0x00}, 1, 0},                                    // 3-gamma off
    {0x26, (uint8_t[]){0x01}, 1, 0},                                    // gamma curve 1
    {0xE0, (uint8_t[]){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7,
                       0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00}, 15, 0},  // positive gamma
    {0xE1, (uint8_t[]){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
                       0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F}, 15, 0},  // negative gamma
};

esp_lcd_panel_handle_t board_lcd_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t board_lcd_io(void) { return s_io; }
void board_backlight(bool on) { gpio_set_level(PIN_LCD_BL, on); }

esp_err_t board_init(void)
{
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight pin");
    board_backlight(false);                  // dark until there is something to show

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_io),
                        TAG, "panel io");

    ili9341_vendor_config_t vendor = {.init_cmds = s_init, .init_cmds_size = sizeof(s_init) / sizeof(s_init[0])};
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,                 // the LCD's reset is the chip's EN
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_io, &dev, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");   // IPS: inverted
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");
    ESP_LOGI(TAG, "board up: %dx%d ILI9341V", BOARD_LCD_W, BOARD_LCD_H);
    return ESP_OK;
}
