// LVGL on the ILI9341V. LVGL renders 320x240 landscape in 40-row strips; the
// panel's own memory-access control does the rotation, so a strip goes out as it
// is, byte-swapped to the big-endian RGB565 the panel reads. A full frame is
// 150 KB, about 31 ms on the 40 MHz bus.

#include "display.h"
#include "board.h"
#include "board_lcd.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define STRIP_ROWS 40

static const char *TAG = "display";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sent;            // given when a strip's DMA has finished
static display_rot_t s_rot = DISPLAY_ROT_CW;
static bool s_lit;                          // backlight on yet

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The SPI ISR only signals: LVGL is not safe to call from here (an ISR that runs
// while flash is busy, saving settings, can't call code that lives in flash).
static bool IRAM_ATTR strip_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woke);
    return woke == pdTRUE;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px_map)
{
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(a));
    esp_lcd_panel_draw_bitmap(board_lcd_panel(), a->x1, a->y1, a->x2 + 1, a->y2 + 1, px_map);
    xSemaphoreTake(s_sent, portMAX_DELAY);  // the DMA reads px_map until this
    // Light the backlight once a real screen (not LVGL's empty default) is on the glass.
    if (!s_lit && lv_display_flush_is_last(d) && lv_obj_get_child_count(lv_screen_active()) > 0) {
        board_backlight(true);
        s_lit = true;
    }
    lv_display_flush_ready(d);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t wait = 100;
        if (display_lock(-1) == ESP_OK) {
            wait = lv_timer_handler();
            display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : wait > 100 ? 100 : wait));
    }
}

esp_err_t display_start(display_rot_t rot)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_sent = xSemaphoreCreateBinary();
    size_t buf_bytes = BOARD_UI_W * STRIP_ROWS * 2;
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_lock || !s_sent || !buf) return ESP_ERR_NO_MEM;

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = strip_sent};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(board_lcd_io(), &cbs, NULL), TAG, "callbacks");
    if (display_set_rotation(rot) != ESP_OK) display_set_rotation(DISPLAY_ROT_CW);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *d = lv_display_create(BOARD_UI_W, BOARD_UI_H);
    if (!d) return ESP_FAIL;
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    return xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t display_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

// Landscape both ways round: the panel swaps rows and columns (MV) and mirrors one
// axis. CW is upright with the USB-C port on the right (checked on the real board).
// `rot none` would be portrait, which the 320x240 layout isn't.
esp_err_t display_set_rotation(display_rot_t rot)
{
    if (rot == DISPLAY_ROT_NONE) return ESP_ERR_NOT_SUPPORTED;
    esp_lcd_panel_handle_t p = board_lcd_panel();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(p, true), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(p, rot == DISPLAY_ROT_CW, rot == DISPLAY_ROT_CCW), TAG, "mirror");
    s_rot = rot;                                                    // the caller redraws everything
    return ESP_OK;
}

display_rot_t display_get_rotation(void) { return s_rot; }
