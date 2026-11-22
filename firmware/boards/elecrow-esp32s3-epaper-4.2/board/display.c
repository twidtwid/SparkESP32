// LVGL on the e-paper. LVGL renders the UI on a 400x300 RGB565 canvas; the flush turns
// each pixel black or white into the panel's 1-bit frame, and the last flush of a
// refresh hands that frame to the e-paper task, which puts it on the glass:
//
//   - a frame identical to the one on the glass is skipped (most redraws change
//     nothing a reader would see);
//   - otherwise a partial refresh (the DU waveform, no flash) changes only the pixels
//     that differ;
//   - a full refresh (the GC waveform, which flashes) at start-up, after a rotation and
//     once an hour wipes the faint ghosts that partial refreshes leave behind.
//
// The panel takes far longer to refresh than LVGL to render, so the task always
// shows the newest frame and skips any in between. How often new readings reach the
// canvas at all is the UI's business: BOARD_UI_MIN_REFRESH_MS in board_config.h.

#include "display.h"
#include "board.h"
#include "board_epd.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display";

#define FULL_EVERY_MS   (60 * 60 * 1000)    // the ghost wipe
#define RETRY_MS        5000                // after a refresh failed

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_epd_task;
static uint8_t *s_frame;                    // LVGL's side: what it rendered (under s_lock)
static uint8_t *s_next;                     // the e-paper task's copy of it
static uint8_t *s_glass;                    // what the panel shows
// Full-refresh requests (start-up, a rotation, a failed refresh) and the last one served,
// under s_lock. A counter, not a flag: a request that arrives while a full refresh runs
// stays pending when that refresh completes. 1 vs 0: the panel shows whatever it showed
// before boot.
static uint32_t s_full_req = 1, s_full_done;
static display_rot_t s_rot = DISPLAY_ROT_CW;

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// A pixel of the canvas into the frame. CW is upright, CCW the same turned 180 degrees.
static inline void put_px(int x, int y, bool white)
{
    if (s_rot == DISPLAY_ROT_CCW) {
        x = (EPD_W - 1) - x;
        y = (EPD_H - 1) - y;
    }
    uint8_t *b = &s_frame[y * EPD_ROW_BYTES + (x >> 3)];
    uint8_t bit = 0x80 >> (x & 7);
    if (white) *b |= bit;
    else *b &= ~bit;
}

// Black or white by luminance (Rec. 601 weights on 6-bit channels). Only nearly
// white (85%) stays white, as on the RLCD: anti-aliased edges go black, so strokes
// come out bold enough to read across a room.
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
    if (lv_display_flush_is_last(d)) xTaskNotifyGive(s_epd_task);
    lv_display_flush_ready(d);
}

static void epd_task(void *arg)
{
    int64_t last_full_us = 0;
    bool pending = false;                    // a frame came that isn't on the glass yet
    while (1) {
        // wake for a new frame, or now and then for the hourly wipe; a frame still
        // pending (its refresh failed) goes again straight away
        if (ulTaskNotifyTake(pdTRUE, pending ? 0 : pdMS_TO_TICKS(60 * 1000))) pending = true;
        bool due = esp_timer_get_time() - last_full_us >= (int64_t)FULL_EVERY_MS * 1000;
        if (!pending && !due) continue;

        bool full;
        uint32_t req;
        if (display_lock(-1) != ESP_OK) continue;
        memcpy(s_next, s_frame, EPD_FRAME_BYTES);
        req = s_full_req;
        full = req != s_full_done || due;
        display_unlock();

        if (!full && memcmp(s_next, s_glass, EPD_FRAME_BYTES) == 0) {
            pending = false;                 // nothing a reader would see changed
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = epd_show(s_next, s_glass, full ? EPD_FULL : EPD_PARTIAL);
        if (err != ESP_OK) {
            // The panel's state is unknown now: start it again and show everything.
            ESP_LOGE(TAG, "%s refresh failed (%s); retrying", full ? "full" : "partial", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            epd_reinit();
            if (display_lock(-1) == ESP_OK) { s_full_req++; display_unlock(); }
            continue;                        // `pending` stays set: try again at once
        }
        memcpy(s_glass, s_next, EPD_FRAME_BYTES);
        pending = false;
        if (full) {
            last_full_us = esp_timer_get_time();
            if (display_lock(-1) == ESP_OK) { s_full_done = req; display_unlock(); }   // later requests stay
            ESP_LOGI(TAG, "full refresh: %d ms", (int)((last_full_us - t0) / 1000));
        }
        // A frame that arrived during the refresh left a notification: the loop takes it next.
    }
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
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_next = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_glass = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    size_t canvas_bytes = BOARD_UI_W * BOARD_UI_H * 2;
    void *canvas = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);   // 234 KB: PSRAM
    if (!s_lock || !s_frame || !s_next || !s_glass || !canvas) return ESP_ERR_NO_MEM;
    if (display_set_rotation(rot) != ESP_OK) s_rot = DISPLAY_ROT_CW;
    memset(s_frame, 0xFF, EPD_FRAME_BYTES);                              // white
    memset(s_glass, 0xFF, EPD_FRAME_BYTES);

    // the e-paper task first: LVGL's first flush notifies it
    if (xTaskCreatePinnedToCore(epd_task, "epd", 4096, NULL, 4, &s_epd_task, 1) != pdPASS) return ESP_FAIL;

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

// cw: upright (the default); ccw: the same turned 180 degrees. none (portrait) isn't
// offered: the layout is landscape.
esp_err_t display_set_rotation(display_rot_t rot)
{
    if (rot == DISPLAY_ROT_NONE) return ESP_ERR_NOT_SUPPORTED;
    if (rot != s_rot) s_full_req++;          // every pixel moves: wipe rather than ghost
    s_rot = rot;                             // the caller redraws everything
    return ESP_OK;
}

display_rot_t display_get_rotation(void) { return s_rot; }
