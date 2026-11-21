// LVGL bring-up on the AMOLED, plus software rotation in the flush path.
//
// Nothing below LVGL can rotate this panel: the SH8601 driver rejects
// swap_xy (no MADCTL row/column exchange), the adapter doesn't rotate
// SPI-class panels, and LVGL's own rotation is bypassed in the adapter's
// render mode. So we chain the flush callback: each rendered strip is
// rotated into a DMA scratch buffer, its area remapped into panel space,
// and the result forwarded to the adapter's original flush.

#include "display.h"
#include "board_lcd.h"

#include <assert.h>

#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "lvgl_private.h"   // display->flush_cb: no public getter in LVGL 9.x

#define STRIP_LINES 40      // partial render buffer: 480*40*2 = 37.5 KB (no PSRAM)

static lv_display_t *s_disp;
static lv_display_flush_cb_t s_panel_flush;
static uint16_t *s_rot_buf;
static display_rot_t s_rot;

static void rotate_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
    if (s_rot == DISPLAY_ROT_NONE) {
        s_panel_flush(d, area, px_map);
        return;
    }
    const int L = BOARD_LCD_W;            // square panel
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    assert(w * h <= L * STRIP_LINES);     // an area never holds more pixels than the draw buffer
    // LVGL may pad draw-buffer rows (LV_DRAW_BUF_STRIDE_ALIGN): walk the
    // source at its real stride, not at w.
    lv_draw_buf_t *db = d->buf_act;
    int src_stride = (int)(db->header.stride / 2);
    const uint16_t *src = (const uint16_t *)px_map;
    lv_area_t out;
    if (s_rot == DISPLAY_ROT_CCW) {
        // logical (x,y) -> panel (y, L-1-x); output strip is h wide, w tall
        out.x1 = area->y1;
        out.x2 = area->y2;
        out.y1 = (L - 1) - area->x2;
        out.y2 = (L - 1) - area->x1;
        for (int y = 0; y < h; y++) {
            const uint16_t *row = src + y * src_stride;
            uint16_t *col = s_rot_buf + y;
            for (int x = 0; x < w; x++) col[(w - 1 - x) * h] = row[x];
        }
    } else {
        // logical (x,y) -> panel (L-1-y, x)
        out.x1 = (L - 1) - area->y2;
        out.x2 = (L - 1) - area->y1;
        out.y1 = area->x1;
        out.y2 = area->x2;
        for (int y = 0; y < h; y++) {
            const uint16_t *row = src + y * src_stride;
            uint16_t *col = s_rot_buf + (h - 1 - y);
            for (int x = 0; x < w; x++) col[x * h] = row[x];
        }
    }
    // esp_lvgl_adapter >= 0.7 compacts each strip in place using the draw
    // buffer's stride before sending it. Our rotated strip is tightly packed
    // at the *rotated* width (h pixels), so report that stride for the
    // hand-off and restore LVGL's afterwards. The adapter reads it
    // synchronously, before this call returns.
    uint32_t saved = db->header.stride;
    db->header.stride = (uint32_t)h * 2;
    s_panel_flush(d, &out, (uint8_t *)s_rot_buf);
    db->header.stride = saved;
}

// The SH8601 needs window starts on even and ends on odd coordinates. With
// L = 480 that parity survives the rotation remap above.
static void round_area_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

esp_err_t display_start(display_rot_t rot)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = 20 * 1024;
    cfg.task_priority = 10;
    cfg.task_core_id = 0;
    cfg.stack_in_psram = false;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    esp_lv_adapter_display_config_t dc = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
        board_lcd_panel(), board_lcd_io(), BOARD_LCD_W, BOARD_LCD_H, ESP_LV_ADAPTER_ROTATE_0);
    dc.profile.buffer_height = STRIP_LINES;
    s_disp = esp_lv_adapter_register_display(&dc);
    if (!s_disp) return ESP_FAIL;
    lv_display_add_event_cb(s_disp, round_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    s_rot_buf = heap_caps_malloc(BOARD_LCD_W * STRIP_LINES * 2, MALLOC_CAP_DMA);
    if (!s_rot_buf) return ESP_ERR_NO_MEM;
    s_rot = rot;
    s_panel_flush = s_disp->flush_cb;
    lv_display_set_flush_cb(s_disp, rotate_flush_cb);
    return esp_lv_adapter_start();
}

esp_err_t display_lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms); }
void display_unlock(void) { esp_lv_adapter_unlock(); }

esp_err_t display_set_rotation(display_rot_t rot)   // square panel: all three work
{
    s_rot = rot;
    return ESP_OK;
}

display_rot_t display_get_rotation(void) { return s_rot; }
