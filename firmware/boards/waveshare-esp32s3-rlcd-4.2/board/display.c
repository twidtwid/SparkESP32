// LVGL on the reflective LCD. LVGL renders the UI on a 400x300 RGB565 canvas;
// the flush turns each pixel black or white and packs it straight into the
// panel's 1-bit frame, rotated to landscape, and the last flush of a refresh
// sends that frame. A whole frame is 15 KB, about 12 ms on the 10 MHz bus.

#include "display.h"
#include "board.h"
#include "board_rlcd.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static SemaphoreHandle_t s_lock;
static uint8_t *s_frame;                     // the panel's 1-bit frame (DMA-capable)
static display_rot_t s_rot = DISPLAY_ROT_CW;

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// A pixel of the 400x300 canvas into the panel's frame. CW is upright with the
// board on its stand (checked on the real board); CCW is the same turned 180 degrees.
static inline void put_px(int x, int y, bool white)
{
    int c = s_rot == DISPLAY_ROT_CW ? y : (BOARD_UI_H - 1) - y;     // panel column, 0..299
    int r = s_rot == DISPLAY_ROT_CW ? (BOARD_UI_W - 1) - x : x;     // panel row, 0..399
    uint32_t i = (uint32_t)(r >> 1) * (RLCD_COLS / 4) + (c >> 2);
    uint8_t bit = 0x80 >> (((c & 3) << 1) | (r & 1));
    if (white) s_frame[i] |= bit;
    else s_frame[i] &= ~bit;
}

// Black or white by luminance (Rec. 601 weights on 6-bit channels). Only nearly
// white (85%) stays white: text's anti-aliased edges go black, so strokes come out
// bolder. Split at half, small text looked thin on the real panel.
static inline bool is_white(uint16_t px)
{
    unsigned r = (px >> 11) << 1, g = (px >> 5) & 0x3F, b = (px & 0x1F) << 1;
    return r * 77 + g * 150 + b * 29 >= 63 * 218;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px_map)
{
    int w = lv_area_get_width(a);
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565) / 2;
    const uint16_t *row = (const uint16_t *)px_map;
    for (int y = a->y1; y <= a->y2; y++, row += stride)
        for (int x = 0; x < w; x++)
            put_px(a->x1 + x, y, is_white(row[x]));
    if (lv_display_flush_is_last(d)) rlcd_send_frame(s_frame);
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
    s_frame = heap_caps_malloc(RLCD_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t canvas_bytes = BOARD_UI_W * BOARD_UI_H * 2;
    void *canvas = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);   // 234 KB: PSRAM
    if (!s_lock || !s_frame || !canvas) return ESP_ERR_NO_MEM;
    if (display_set_rotation(rot) != ESP_OK) s_rot = DISPLAY_ROT_CW;
    memset(s_frame, 0xFF, RLCD_FRAME_BYTES);                            // white
    rlcd_send_frame(s_frame);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *d = lv_display_create(BOARD_UI_W, BOARD_UI_H);
    if (!d) return ESP_FAIL;
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, canvas, NULL, canvas_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    return xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t display_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

esp_err_t display_set_rotation(display_rot_t rot)
{
    if (rot == DISPLAY_ROT_NONE) return ESP_ERR_NOT_SUPPORTED;   // portrait: the layout is landscape
    s_rot = rot;                                                 // the caller redraws everything
    return ESP_OK;
}

display_rot_t display_get_rotation(void) { return s_rot; }
