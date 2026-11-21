// USB-serial console: the vitals feed, setup, and the QA channel.
//
//   stat <cpu> <gpu> <mem_used_mb> <mem_total_mb> <temp_c> <power_dw> <cores> <host>
//                      one sample (sparkesp32-agent pushes these 1/s over USB)
//   name <text>        header title, saved (quote it: name "Lab Spark"; name "" = hostname)
//   rot none|ccw|cw    screen rotation, saved
//   wifi [show] | ssids <a,b> | pass <pw> | url <url> | scan | clear
//   status             machine-readable state; first line identifies the firmware
//   demo [load]        synthetic sample for bench QA
//   shot               CRC'd base64 framebuffer dump (tools/devctl.py reassembles)

#include "console_cmds.h"
#include "board.h"
#include "display.h"
#include "net.h"
#include "settings.h"
#include "spark_data.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include "lvgl.h"
#include "lvgl_private.h"
#include "mbedtls/base64.h"

// ---------------------------------------------------------------- shot

static lv_display_flush_cb_t s_orig_flush;

static void tee_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    size_t nbytes = (size_t)w * h * 2;
    uint32_t crc = esp_crc32_le(0, px_map, nbytes);
    printf("AREA %d %d %d %d %u %08lx\n", (int)area->x1, (int)area->y1, (int)area->x2,
           (int)area->y2, (unsigned)nbytes, (unsigned long)crc);
    static unsigned char line[97];
    const unsigned char *src = px_map;
    for (size_t left = nbytes, n = 0; left > 0; n++) {
        size_t chunk = left > 72 ? 72 : left, olen = 0;
        mbedtls_base64_encode(line, sizeof(line), &olen, src, chunk);
        fwrite(line, 1, olen, stdout);
        fputc('\n', stdout);
        src += chunk;
        left -= chunk;
        // A whole-screen area is hundreds of KB of text: let the idle task in now
        // and then, or the task watchdog fires mid-dump.
        if ((n & 63) == 63) vTaskDelay(1);
    }
    s_orig_flush(disp, area, px_map);
}

// The dump is taken in logical (pre-rotation) orientation, before the panel's
// byte swap or 1-bit conversion: exactly what LVGL rendered.
static int cmd_shot(int argc, char **argv)
{
    if (display_lock(5000) != ESP_OK) { printf("ERR lock\n"); return 1; }
    esp_log_level_t prev = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    ui_update();
    lv_display_t *disp = lv_display_get_default();
    s_orig_flush = disp->flush_cb;
    lv_display_set_flush_cb(disp, tee_flush_cb);
    printf("SHOT-BEGIN %d %d rgb565\n", (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    printf("SHOT-END\n");
    lv_display_set_flush_cb(disp, s_orig_flush);
    esp_log_level_set("*", prev);
    display_unlock();
    return 0;
}

// ---------------------------------------------------------------- data

static int cmd_stat(int argc, char **argv)
{
    const char *why = NULL;
    if (display_lock(3000) != ESP_OK) { printf("ERR lock\n"); return 1; }
    int rc = spark_apply_args(argc, argv, SRC_USB, &why);
    if (rc == 0) ui_update();
    display_unlock();
    if (rc != 0) {
        printf("ERR stat %s\n", why);
        return 1;
    }
    printf("OK stat\n");
    return 0;
}

static int cmd_demo(int argc, char **argv)
{
    int load = argc > 1 ? atoi(argv[1]) : 42;
    if (display_lock(3000) != ESP_OK) { printf("ERR lock\n"); return 1; }
    spark_demo(load);
    ui_update();
    display_unlock();
    printf("OK demo %d\n", load);
    return 0;
}

// ---------------------------------------------------------------- config

static int cmd_name(int argc, char **argv)
{
    if (argc != 2) { printf("ERR usage: name \"<text>\"\n"); return 1; }
    const char *n = argv[1];
    if (strlen(n) > UI_TITLE_MAX) { printf("ERR name longer than %d chars\n", UI_TITLE_MAX); return 1; }
    for (const char *c = n; *c; c++)
        if (!isprint((unsigned char)*c)) { printf("ERR name must be printable ASCII\n"); return 1; }
    if (settings_set("name", n) != ESP_OK) { printf("ERR name save\n"); return 1; }
    if (display_lock(3000) == ESP_OK) { ui_set_name(n); display_unlock(); }
    printf("OK name\n");
    return 0;
}

static int cmd_rot(int argc, char **argv)
{
    if (argc < 2) { printf("OK rot %s\n", display_rot_name(display_get_rotation())); return 0; }
    int r = display_rot_from_name(argv[1]);
    if (r < 0) { printf("ERR usage: rot none|ccw|cw\n"); return 1; }
    if (display_lock(5000) != ESP_OK) { printf("ERR lock\n"); return 1; }
    if (display_set_rotation((display_rot_t)r) != ESP_OK) {
        display_unlock();
        printf("ERR rot %s not supported on this board\n", argv[1]);
        return 1;
    }
    if (settings_set("rot", argv[1]) != ESP_OK) { display_unlock(); printf("ERR rot save\n"); return 1; }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_display_get_default());
    display_unlock();
    printf("OK rot\n");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "show";
    esp_err_t err;
    // The REPL added this line to its history before running it. For `pass`, drop
    // it whatever happens next, so an up-arrow can't recall the passphrase, even
    // from a mistyped command.
    if (!strcmp(sub, "pass")) linenoiseHistoryFree();
    if (!strcmp(sub, "show")) { net_show(); return 0; }
    if (!strcmp(sub, "scan")) return net_scan_print() == ESP_OK ? 0 : 1;
    if (!strcmp(sub, "clear")) {
        err = net_clear();
    } else if (argc == 3 && !strcmp(sub, "ssids")) {
        err = net_set_ssids(argv[2]);
    } else if (argc == 3 && !strcmp(sub, "pass")) {
        err = net_set_pass(argv[2]);
    } else if (argc == 3 && !strcmp(sub, "url")) {
        err = net_set_url(argv[2]);
    } else {
        printf("ERR usage: wifi [show] | ssids <a,b> | pass <pw> | url <url> | scan | clear\n");
        return 1;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        printf("ERR wifi %s invalid (ssids: <=4, each 1-32 bytes; pass: 8-63 chars or empty; "
               "url: http://host[:port]/stat)\n", sub);
        return 1;
    }
    if (err != ESP_OK) { printf("ERR wifi %s %s\n", sub, esp_err_to_name(err)); return 1; }
    printf("OK wifi %s\n", sub);      // never echoes the passphrase
    return 0;
}

// ---------------------------------------------------------------- status

static unsigned stack_free(const char *task)
{
    TaskHandle_t t = xTaskGetHandle(task);
    return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0;
}

// a field for the status line: the number, or "-" when unknown (as on the wire)
static const char *fv(char buf[16], int32_t v)
{
    if (v == SPARK_UNKNOWN) return "-";
    snprintf(buf, 16, "%ld", (long)v);
    return buf;
}

static int cmd_status(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char name[UI_TITLE_MAX + 1];
    settings_get("name", name, sizeof(name), "");
    printf("STATUS-BEGIN\n");
    printf("fw=sparkesp32 version=%s idf=%s board=%s\n", app->version, app->idf_ver, BOARD_ID);  // handshake
    printf("name='%s' rot=%s\n", name, display_rot_name(display_get_rotation()));
    printf("heap_free=%u min=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    printf("stack_free repl=%u net=%u tick=%u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL),
           stack_free("net"), stack_free("tick"));
    if (display_lock(2000) == ESP_OK) {
        spark_t s = g_spark;
        uint32_t age = spark_age_ms();
        status_t st = ui_status();
        display_unlock();
        char b[6][16];
        printf("valid=%d age_ms=%ld src=%s host=%s cpu=%s gpu=%s mem=%s/%sMB temp=%s power_dw=%s "
               "cores=%d\n", s.valid, s.valid ? (long)age : -1L, spark_src_name(s.source),
               s.host[0] ? s.host : "-", fv(b[0], s.cpu), fv(b[1], s.gpu), fv(b[2], s.mem_used_mb),
               fv(b[3], s.mem_total_mb), fv(b[4], s.temp_c), fv(b[5], s.power_dw), s.ncores);
        printf("screen=%s reason='%s'\n", screen_name(st.screen), st.reason);
    }
    printf("wifi configured=%d connected=%d url=%d\n", net_wifi_configured(),
           net_wifi_is_connected(), net_url_configured());
    printf("STATUS-END\n");
    return 0;
}

// ---------------------------------------------------------------- start

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "spark>";
    rc.max_cmdline_length = 256;     // a 32-core stat line with a 63-char host is ~200 chars
    rc.task_stack_size = 8192;       // shot/rot render LVGL on this task

    const esp_console_cmd_t cmds[] = {
        { .command = "stat",   .help = "feed one sample",          .func = cmd_stat },
        { .command = "name",   .help = "set the header title",     .func = cmd_name },
        { .command = "rot",    .help = "rotation none|ccw|cw",     .func = cmd_rot },
        { .command = "wifi",   .help = "wifi config / scan",       .func = cmd_wifi },
        { .command = "status", .help = "state (handshake line 1)", .func = cmd_status },
        { .command = "demo",   .help = "synthetic sample",         .func = cmd_demo },
        { .command = "shot",   .help = "dump framebuffer",         .func = cmd_shot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));

    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
