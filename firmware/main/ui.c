// SparkESP32 UI: DGX Spark vitals on the board's screen.
// Three ring gauges (CPU / GPU with its clock / unified memory), a per-core heatmap,
// temperature + power tiles, and a liveness state that doubles as the
// Spark's missing power light. When the agent reports the GB10's stuck-clock state,
// the GPU ring says so. Every lv_* call here runs under display_lock().
//
// The board says how big the canvas is (BOARD_UI_W/H) and whether it's black and
// white (BOARD_DISPLAY_MONO); this file picks the matching layout and theme. A
// new screen size needs a layout below (docs/adding-a-device.md). A board whose
// rotations change the canvas's shape (the e-paper, wide or tall) has a layout for
// each, and the UI is rebuilt when the shape changes (ui_rebuild).

#include "ui.h"
#include "board.h"
#include "net.h"
#include "spark_data.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

// ---------------------------------------------------------------- layout

typedef struct {
    int w, h;                               // the canvas this layout is for
    int margin;                             // page side margin
    int title_y, title_w;                   // the Spark's name, top left
    const lv_font_t *title_font;
    int status_y, reason_y, dot;            // state + reason, top right; the dot's size
    int ring_r, ring_w;                     // three ring gauges
    int ring_cx[3], ring_cy[3];             // each one's center
    int ring_val_dy, ring_sub_dy;
    int ring_title_dy;                      // title's top, from the height of the arc's two ends
    const lv_font_t *ring_val_font;         // the big % in each ring
    const lv_font_t *ring_sub_font;         // the detail under it: "2405 MHz", "99/131G"
    const char *gpu_mhz_fmt;                // the GPU clock's format in that detail line
    int cores_y, cells_y, cells_h, cells_gap;   // the heatmap row
    int tile_x[2], tile_y[2], tile_w, tile_h;   // temperature and power
    int tile_radius, tile_pad, tile_label_y, tile_value_y;
    const lv_font_t *tile_label_font, *tile_value_font;
} layout_t;

#if BOARD_UI_W == 480 && BOARD_UI_H == 480        // the C6 AMOLED: square, roomy
static const layout_t LAYOUTS[] = {{
    .w = 480, .h = 480,
    .margin = 22, .title_y = 14, .title_w = 280, .title_font = &lv_font_montserrat_28,
    .status_y = 22, .reason_y = 46, .dot = 14,
    .ring_r = 58, .ring_w = 10, .ring_cx = {92, 240, 388}, .ring_cy = {150, 150, 150},
    .ring_val_dy = -6, .ring_sub_dy = 16, .ring_title_dy = 23, .ring_val_font = &lv_font_montserrat_28,
    .ring_sub_font = &lv_font_montserrat_14, .gpu_mhz_fmt = "%d MHz",
    .cores_y = 250, .cells_y = 276, .cells_h = 30, .cells_gap = 3,
    .tile_x = {16, 248}, .tile_y = {322, 322}, .tile_w = 216, .tile_h = 142,
    .tile_radius = 14, .tile_pad = 16, .tile_label_y = 14, .tile_value_y = -12,
    .tile_label_font = &lv_font_montserrat_16, .tile_value_font = &lv_font_montserrat_48,
}};
#elif BOARD_UI_W == 400 && BOARD_UI_H == 300      // 4.2" landscape (the S3 RLCD)
static const layout_t LAYOUTS[] = {{
    .w = 400, .h = 300,
    .margin = 14, .title_y = 8, .title_w = 220, .title_font = &lv_font_montserrat_28,
    .status_y = 12, .reason_y = 34, .dot = 12,
    .ring_r = 52, .ring_w = 7, .ring_cx = {72, 200, 328}, .ring_cy = {116, 116, 116},
    .ring_val_dy = -5, .ring_sub_dy = 14, .ring_title_dy = -7, .ring_val_font = &lv_font_montserrat_28,
    .ring_sub_font = &lv_font_montserrat_14, .gpu_mhz_fmt = "%d MHz",
    .cores_y = 182, .cells_y = 200, .cells_h = 22, .cells_gap = 2,
    .tile_x = {14, 206}, .tile_y = {230, 230}, .tile_w = 180, .tile_h = 62,
    .tile_radius = 8, .tile_pad = 10, .tile_label_y = 5, .tile_value_y = -4,
    .tile_label_font = &lv_font_montserrat_16, .tile_value_font = &lv_font_montserrat_28,
}};
#elif BOARD_UI_W == 320 && BOARD_UI_H == 240      // 2.8" landscape (the S3 TFT): tight
static const layout_t LAYOUTS[] = {{
    .w = 320, .h = 240,
    .margin = 10, .title_y = 8, .title_w = 170, .title_font = &lv_font_montserrat_24,
    .status_y = 8, .reason_y = 28, .dot = 10,
    .ring_r = 42, .ring_w = 6, .ring_cx = {56, 160, 264}, .ring_cy = {92, 92, 92},
    .ring_val_dy = -5, .ring_sub_dy = 12, .ring_title_dy = -4, .ring_val_font = &lv_font_montserrat_24,
    .ring_sub_font = &lv_font_montserrat_12, .gpu_mhz_fmt = "%dMHz",   // a 4-digit clock fits this small ring
    .cores_y = 152, .cells_y = 168, .cells_h = 14, .cells_gap = 1,
    .tile_x = {10, 164}, .tile_y = {186, 186}, .tile_w = 146, .tile_h = 52,
    .tile_radius = 8, .tile_pad = 8, .tile_label_y = 3, .tile_value_y = -2,
    .tile_label_font = &lv_font_montserrat_14, .tile_value_font = &lv_font_montserrat_28,
}};
#elif BOARD_UI_W == 792 && BOARD_UI_H == 272      // 5.79" e-paper (the Elecrow): wide, or tall
// "100%" in the rings' 36 px is 92 px wide and clears the arc by 5 px; a tile is as wide
// as "140.0 W" in 36 px (144) plus its padding.
static const layout_t LAYOUTS[] = {{
    // wide (rot cw): one row of rings, then temperature and power; the cores along the bottom
    .w = 792, .h = 272,
    .margin = 16, .title_y = 6, .title_w = 440, .title_font = &lv_font_montserrat_28,
    .status_y = 10, .reason_y = 32, .dot = 12,
    .ring_r = 64, .ring_w = 9, .ring_cx = {80, 218, 356}, .ring_cy = {128, 128, 128},
    .ring_val_dy = -6, .ring_sub_dy = 24, .ring_title_dy = -6, .ring_val_font = &lv_font_montserrat_36,
    .ring_sub_font = &lv_font_montserrat_16, .gpu_mhz_fmt = "%d MHz",
    .cores_y = 204, .cells_y = 226, .cells_h = 34, .cells_gap = 4,
    .tile_x = {432, 608}, .tile_y = {64, 64}, .tile_w = 168, .tile_h = 128,
    .tile_radius = 10, .tile_pad = 12, .tile_label_y = 10, .tile_value_y = -10,
    .tile_label_font = &lv_font_montserrat_16, .tile_value_font = &lv_font_montserrat_36,
}, {
    // tall (rot ccw): the board on its short end; the rings stacked, the tiles under the cores
    .w = 272, .h = 792,
    .margin = 16, .title_y = 12, .title_w = 240, .title_font = &lv_font_montserrat_28,
    .status_y = 56, .reason_y = 78, .dot = 12,
    .ring_r = 64, .ring_w = 9, .ring_cx = {136, 136, 136}, .ring_cy = {170, 314, 458},
    .ring_val_dy = -6, .ring_sub_dy = 24, .ring_title_dy = -6, .ring_val_font = &lv_font_montserrat_36,
    .ring_sub_font = &lv_font_montserrat_16, .gpu_mhz_fmt = "%d MHz",
    .cores_y = 534, .cells_y = 556, .cells_h = 40, .cells_gap = 2,
    .tile_x = {16, 16}, .tile_y = {610, 700}, .tile_w = 240, .tile_h = 82,
    .tile_radius = 10, .tile_pad = 12, .tile_label_y = 8, .tile_value_y = -8,
    .tile_label_font = &lv_font_montserrat_16, .tile_value_font = &lv_font_montserrat_36,
}};
#else
#error "no UI layout for this board's BOARD_UI_W x BOARD_UI_H: add one here (docs/adding-a-device.md)"
#endif

static const layout_t *L = &LAYOUTS[0];     // the one for the canvas's current shape (ui_init)

static const layout_t *layout_for(int w, int h)
{
    for (size_t i = 0; i < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); i++)
        if (LAYOUTS[i].w == w && LAYOUTS[i].h == h) return &LAYOUTS[i];
    return &LAYOUTS[0];
}

// ---------------------------------------------------------------- theme

#if BOARD_DISPLAY_MONO
// Black and white only: anything grey lands on one side or the other, so the
// mono theme says with shape what the color theme says with color.
#define C_BG     lv_color_hex(0xFFFFFF)
#define C_CARD   lv_color_hex(0xFFFFFF)
#define C_TRACK  lv_color_hex(0x000000)
#define C_GREEN  lv_color_hex(0x000000)
#define C_WHITE  lv_color_hex(0x000000)
#define C_DIM    lv_color_hex(0x000000)
#define C_AMBER  lv_color_hex(0x000000)
#define TRACK_W  2                          // a thin track; the value's arc is thick
#else
#define C_BG     lv_color_hex(0x0A0A0A)
#define C_CARD   lv_color_hex(0x161616)
#define C_TRACK  lv_color_hex(0x242424)
#define C_GREEN  lv_color_hex(0x76B900)     // NVIDIA green
#define C_WHITE  lv_color_hex(0xF2F2F2)
#define C_DIM    lv_color_hex(0x8A8A8A)
#define C_AMBER  lv_color_hex(0xF0A030)
#define TRACK_W  L->ring_w
#endif

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *val;    // big % in the center
    lv_obj_t *sub;    // small detail under the value ("99/131G", "2405 MHz")
    lv_obj_t *title;  // under the ring ("GPU")
} ring_t;

static lv_obj_t *s_scr, *s_name;
static lv_obj_t *s_dot, *s_livetext, *s_srctext;
static ring_t    s_cpu, s_gpu, s_mem;
static lv_obj_t *s_cells[SPARK_MAX_CORES];
#if BOARD_DISPLAY_MONO
static lv_obj_t *s_fills[SPARK_MAX_CORES];  // mono heatmap: each cell fills from the bottom
#endif
static int       s_ncells;
static lv_obj_t *s_temp_val, *s_pow_val;
static const char *s_shown_label, *s_shown_reason;   // the header at the last redraw
static uint32_t    s_shown_ms;

#if !BOARD_DISPLAY_MONO
// 0..100 load -> green (idle) -> yellow -> red (pinned)
static lv_color_t heat_color(int pct)
{
    if (pct < 55) return lv_color_make(0x40 + pct * (0xE0 - 0x40) / 55, 0xB9, 0x10);
    return lv_color_make(0xE0 + (pct - 55) * (0xFF - 0xE0) / 45,
                         0xB9 - (pct - 55) * (0xB9 - 0x30) / 45, 0x10);
}
#endif

static lv_obj_t *mklabel(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    return l;
}

// ring gauge i, centered at the layout's (cx,cy) with radius r, titled below
static void make_ring(ring_t *rg, int i, const char *title)
{
    int cx = L->ring_cx[i], cy = L->ring_cy[i], r = L->ring_r;
    lv_obj_t *arc = lv_arc_create(s_scr);
    lv_obj_set_size(arc, r * 2, r * 2);
    lv_obj_set_pos(arc, cx - r, cy - r);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);             // its labels (below) must never scroll it
    lv_obj_set_style_arc_width(arc, TRACK_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, L->ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_GREEN, LV_PART_INDICATOR);
    rg->arc = arc;

    // children of the arc, aligned once: LVGL re-centres them when the text changes
    rg->val = mklabel(arc, L->ring_val_font, C_WHITE, "--");
    lv_obj_align(rg->val, LV_ALIGN_CENTER, 0, L->ring_val_dy);
    rg->sub = mklabel(arc, L->ring_sub_font, C_DIM, "");
    lv_obj_align(rg->sub, LV_ALIGN_CENTER, 0, L->ring_sub_dy);

    // In the gap at the bottom of the arc: its two ends sit r*sin(45) below the
    // center. TOP_MID centers on the screen; offset by (cx - middle) for this ring.
    lv_obj_t *t = mklabel(s_scr, &lv_font_montserrat_16, C_DIM, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, cx - L->w / 2, cy + r * 707 / 1000 + L->ring_title_dy);
    rg->title = t;
}

#if BOARD_DISPLAY_MONO
// White on black instead of black on white: the mono theme's way of saying "look here".
static void set_inverted(lv_obj_t *label, bool on, lv_color_t normal)
{
    lv_obj_set_style_bg_color(label, C_TRACK, 0);
    lv_obj_set_style_bg_opa(label, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(label, on ? 3 : 0, 0);
    lv_obj_set_style_text_color(label, on ? C_BG : normal, 0);
}
#endif

// The GB10 stuck-clock state (the agent's verdict, docs/troubleshooting.md). The header's
// second line says so; the GPU ring's arc, clock and title turn amber, or on a
// black-and-white screen the warning and the GPU title are inverted. Nothing moves, so
// nothing can run into a ring.
static int s_warning_shown = -1;                   // skip the restyle when nothing changed

static void set_gpu_warning(bool on)
{
    if (s_warning_shown == on) return;
    s_warning_shown = on;
#if BOARD_DISPLAY_MONO
    set_inverted(s_gpu.title, on, C_DIM);
    set_inverted(s_srctext, on, C_DIM);
#else
    lv_obj_set_style_text_color(s_gpu.title, on ? C_AMBER : C_DIM, 0);
    lv_obj_set_style_text_color(s_gpu.sub, on ? C_AMBER : C_DIM, 0);
    lv_obj_set_style_text_color(s_srctext, on ? C_AMBER : C_DIM, 0);
#endif
}

// readout tile i: label + big value
static lv_obj_t *make_tile(int i, const char *label)
{
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, L->tile_x[i], L->tile_y[i]);
    lv_obj_set_size(card, L->tile_w, L->tile_h);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, L->tile_radius, 0);
#if BOARD_DISPLAY_MONO
    lv_obj_set_style_border_width(card, 2, 0);     // white on white: the outline is the card
    lv_obj_set_style_border_color(card, C_TRACK, 0);
#endif
    lv_obj_set_scrollable(card, false);

    lv_obj_t *l = mklabel(card, L->tile_label_font, C_DIM, label);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, L->tile_pad, L->tile_label_y);
    lv_obj_t *v = mklabel(card, L->tile_value_font, C_WHITE, "--");
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, L->tile_pad, L->tile_value_y);
    return v;
}

// Title precedence: the saved name, else the Spark's hostname from the feed,
// else a generic label until the first sample arrives.
static char s_saved_name[UI_TITLE_MAX + 1];

static void refresh_title(void)
{
    const char *t = s_saved_name[0] ? s_saved_name : g_spark.host[0] ? g_spark.host : "DGX Spark";
    if (strcmp(lv_label_get_text(s_name), t) != 0) lv_label_set_text(s_name, t);
}

void ui_set_name(const char *name)
{
    strlcpy(s_saved_name, name ? name : "", sizeof(s_saved_name));
    refresh_title();
}

void ui_init(const char *name)
{
    lv_display_t *d = lv_display_get_default();
    L = layout_for(lv_display_get_horizontal_resolution(d), lv_display_get_vertical_resolution(d));
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    // header: title (dot-truncated so it can't run into the status) + liveness
    s_name = mklabel(s_scr, L->title_font, C_WHITE, "");
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_name, L->title_w);
    // one line tall: without a height, DOTS wraps a long name ("dgx-spark-lab-bench-07",
    // "Lab Spark Upstairs Bench") onto more lines, down into the rings, instead of cutting it
    lv_obj_set_height(s_name, lv_font_get_line_height(L->title_font));
    lv_obj_set_pos(s_name, L->margin, L->title_y);
    ui_set_name(name);

    s_dot = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, L->dot, L->dot);
    lv_obj_set_style_radius(s_dot, L->dot / 2, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
#if BOARD_DISPLAY_MONO
    lv_obj_set_style_border_width(s_dot, 2, 0);    // hollow unless ONLINE (see ui_update)
    lv_obj_set_style_border_color(s_dot, C_TRACK, 0);
#endif
    s_livetext = mklabel(s_scr, &lv_font_montserrat_16, C_DIM, "WAITING");
    lv_obj_align(s_livetext, LV_ALIGN_TOP_RIGHT, -L->margin, L->status_y);
    s_srctext = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_srctext, LV_ALIGN_TOP_RIGHT, -L->margin, L->reason_y);

    make_ring(&s_cpu, 0, "CPU");
    make_ring(&s_gpu, 1, "GPU");
    make_ring(&s_mem, 2, "MEM");

    lv_obj_t *ch = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "CPU CORES");
    lv_obj_set_pos(ch, L->margin, L->cores_y);
    for (int i = 0; i < SPARK_MAX_CORES; i++) {
        lv_obj_t *c = lv_obj_create(s_scr);
        lv_obj_remove_style_all(c);
        lv_obj_set_style_radius(c, 3, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
#if !BOARD_DISPLAY_MONO
        lv_obj_set_style_bg_color(c, C_TRACK, 0);
#else
        lv_obj_set_style_bg_color(c, C_BG, 0);     // an outlined cell that fills with load
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, C_TRACK, 0);
        lv_obj_t *f = lv_obj_create(c);
        lv_obj_remove_style_all(f);
        lv_obj_set_style_bg_color(f, C_TRACK, 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_width(f, lv_pct(100));
        lv_obj_set_height(f, 0);
        lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, 0);
        s_fills[i] = f;
#endif
        lv_obj_set_hidden(c, true);
        s_cells[i] = c;
    }

    s_temp_val = make_tile(0, "DEVICE TEMP");
    s_pow_val = make_tile(1, "DEVICE POWER");
}

// one row across the width, inside the page margins
static void layout_cells(int n)
{
    const int x0 = L->margin, right = L->w - L->margin;
    int w = n ? (right - x0 - L->cells_gap * (n - 1)) / n : 0;   // n <= 32 keeps w >= 8
    for (int i = 0; i < SPARK_MAX_CORES; i++) {
        bool show = i < n;
        if (show) {
            lv_obj_set_size(s_cells[i], w, L->cells_h);
            lv_obj_set_pos(s_cells[i], x0 + i * (w + L->cells_gap), L->cells_y);
        }
        lv_obj_set_hidden(s_cells[i], !show);
    }
    s_ncells = n;
}

static void set_ring(ring_t *rg, int32_t pct, bool fresh)
{
    char buf[16];
    bool known = pct != SPARK_UNKNOWN;
    lv_arc_set_value(rg->arc, known ? pct : 0);
    if (known) snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    lv_label_set_text(rg->val, known ? buf : "--");
#if BOARD_DISPLAY_MONO
    lv_obj_set_style_arc_width(rg->arc, fresh ? L->ring_w : TRACK_W, LV_PART_INDICATOR);   // stale: thin
#else
    lv_obj_set_style_arc_color(rg->arc, fresh ? C_GREEN : C_TRACK, LV_PART_INDICATOR);
#endif
}

status_t ui_status(void)
{
    link_t link = net_link();
    status_in_t in = {
        .fresh = spark_age_ms() < SPARK_STALE_MS,
        .ever_fed = g_spark.valid,
        .source = g_spark.source,
        .wifi_configured = net_wifi_configured(),
        .url_configured = net_url_configured(),
        .link = link.state,
        .problem = link_problem(&link),
        .feed = net_feed(),
    };
    return status_of(&in);
}

// A slow screen (e-paper) takes new readings at most this often, from its
// board_config.h; a change in what the header says still goes out at once
// (status_redraw_due). 0: every update redraws.
#ifndef BOARD_UI_MIN_REFRESH_MS
#define BOARD_UI_MIN_REFRESH_MS 0
#endif


static void update(bool now)
{
    char buf[32];
    status_t st = ui_status();                     // the header's state + reason (status.c)
    bool fresh = st.screen == SCREEN_ONLINE;
    bool stuck = fresh && g_spark.gpu_stuck == 1;  // stale data says nothing about now
    const char *reason = stuck ? "CLOCK STUCK" : st.reason;
    uint32_t t = spark_now_ms();
    if (!now && !status_redraw_due(s_shown_label, s_shown_reason, st.label, reason, t - s_shown_ms,
                                   BOARD_UI_MIN_REFRESH_MS))
        return;
    s_shown_label = st.label;
    s_shown_reason = reason;
    s_shown_ms = t;
    lv_color_t col = fresh ? C_GREEN : st.screen == SCREEN_WAITING ? C_DIM : C_AMBER;

    refresh_title();
    lv_label_set_text(s_livetext, st.label);
    lv_obj_set_style_text_color(s_livetext, col, 0);
#if BOARD_DISPLAY_MONO
    lv_obj_set_style_bg_color(s_dot, fresh ? C_TRACK : C_BG, 0);   // filled = ONLINE
#else
    lv_obj_set_style_bg_color(s_dot, col, 0);
#endif
    lv_obj_align_to(s_dot, s_livetext, LV_ALIGN_OUT_LEFT_MID, -10, 0);   // after the text's new width
    lv_label_set_text(s_srctext, reason);
    set_gpu_warning(stuck);

    // fields are SPARK_UNKNOWN until the first sample, so no per-field `valid` checks
    set_ring(&s_cpu, g_spark.cpu, fresh);
    set_ring(&s_gpu, g_spark.gpu, fresh);
    set_ring(&s_mem, spark_mem_pct(), fresh);

    // the GPU's clock under its %; nothing from an agent that doesn't send it (protocol v1)
    if (g_spark.gpu_mhz != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), L->gpu_mhz_fmt, (int)g_spark.gpu_mhz);
        lv_label_set_text(s_gpu.sub, buf);
    } else {
        lv_label_set_text(s_gpu.sub, "");
    }
#if !BOARD_DISPLAY_MONO
    if (stuck) lv_obj_set_style_arc_color(s_gpu.arc, C_AMBER, LV_PART_INDICATOR);
#endif
    if (spark_mem_pct() != SPARK_UNKNOWN) {
        // decimal GB, rounded — matches NVIDIA's dashboard ("99 GB of 131 GB")
        snprintf(buf, sizeof(buf), "%ld/%ldG", (long)((g_spark.mem_used_mb + 500LL) / 1000),
                 (long)((g_spark.mem_total_mb + 500LL) / 1000));
        lv_label_set_text(s_mem.sub, buf);
    } else {
        lv_label_set_text(s_mem.sub, "");
    }

    if (g_spark.ncores != s_ncells) layout_cells(g_spark.ncores);
    for (int i = 0; i < s_ncells; i++) {
#if BOARD_DISPLAY_MONO
        int inner = L->cells_h - 2;                 // inside the 1 px outline
        lv_obj_set_height(s_fills[i], fresh ? inner * g_spark.cores[i] / 100 : 0);
#else
        lv_obj_set_style_bg_color(s_cells[i], fresh ? heat_color(g_spark.cores[i]) : C_TRACK, 0);
#endif
    }

    if (g_spark.temp_c != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)g_spark.temp_c);   // degree sign
        lv_label_set_text(s_temp_val, buf);
    } else {
        lv_label_set_text(s_temp_val, "--");
    }
    if (g_spark.power_dw != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d.%d W", (int)(g_spark.power_dw / 10), (int)(g_spark.power_dw % 10));
        lv_label_set_text(s_pow_val, buf);
    } else {
        lv_label_set_text(s_pow_val, "--");
    }
}

void ui_update(void) { update(false); }
void ui_update_now(void) { update(true); }

void ui_rebuild(void)
{
    char name[UI_TITLE_MAX + 1];
    strlcpy(name, s_saved_name, sizeof(name));     // ui_init copies it back
    lv_obj_clean(lv_screen_active());
    s_ncells = 0;
    s_warning_shown = -1;
    s_shown_label = s_shown_reason = NULL;
    ui_init(name);
    ui_update_now();
}
