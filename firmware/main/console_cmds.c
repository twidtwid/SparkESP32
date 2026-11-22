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
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif
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

// The dump bypasses stdio and goes straight to the console's driver.
//
// On the chip's own USB-Serial/JTAG (most boards), that matters. ESP-IDF's
// console throws away every write() while its USB connection monitor
// (usb_serial_jtag_is_connected(), which watches for the host's start-of-frame
// packets on each FreeRTOS tick) says the host is gone, and still reports
// success: vfs_console's write returns the full size. On the C6 that monitor
// says so for a few milliseconds at a time with the host attached and reading,
// and a full-screen dump through stdio (one write() per base64 line) lost
// every line written meanwhile, with nothing to show for it but a bad CRC.
// The driver call doesn't ask the monitor: it waits until the bytes are queued
// or SHOT_WRITE_TIMEOUT_MS passes, so every line reaches the host or the dump
// ends in ERR. Whole lines also travel in fewer, fuller USB packets than
// stdio's byte-at-a-time path, which makes the dump about 5x faster.
#define SHOT_WRITE_TIMEOUT_MS 1000

static lv_display_flush_cb_t s_orig_flush;
static bool s_shot_ok;      // false once a write timed out: the rest is skipped

// Behind a USB-UART bridge (the console on UART0, e.g. the Elecrow e-paper), the UART
// driver blocks until the bytes are queued and never drops any: the wire runs at the
// baud rate whether or not anyone listens.
static void shot_write(const char *buf, size_t len)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (s_shot_ok && usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(SHOT_WRITE_TIMEOUT_MS)) != (int)len)
        s_shot_ok = false;
#else
    if (s_shot_ok && uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, len) != (int)len)
        s_shot_ok = false;
#endif
}

// one "\r\n"-terminated line, as the console's own output ends its lines
static void shot_line(const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    shot_write(buf, n);
}

static void tee_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    size_t nbytes = (size_t)w * h * 2;
    uint32_t crc = esp_crc32_le(0, px_map, nbytes);
    shot_line("AREA %d %d %d %d %u %08lx", (int)area->x1, (int)area->y1, (int)area->x2,
              (int)area->y2, (unsigned)nbytes, (unsigned long)crc);
    static unsigned char line[96 + 3];     // 72 bytes as base64, then "\r\n" (+ NUL while encoding)
    const unsigned char *src = px_map;
    for (size_t left = nbytes, n = 0; left > 0 && s_shot_ok; n++) {
        size_t chunk = left > 72 ? 72 : left, olen = 0;
        mbedtls_base64_encode(line, sizeof(line), &olen, src, chunk);
        line[olen++] = '\r';
        line[olen++] = '\n';
        shot_write((const char *)line, olen);
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
    ui_update_now();
    lv_display_t *disp = lv_display_get_default();
    s_orig_flush = disp->flush_cb;
    lv_display_set_flush_cb(disp, tee_flush_cb);
    fflush(stdout);                         // the echoed command goes out before the dump
    s_shot_ok = true;
    shot_line("SHOT-BEGIN %d %d rgb565", (int)lv_display_get_horizontal_resolution(disp),
              (int)lv_display_get_vertical_resolution(disp));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    shot_line("SHOT-END");
    lv_display_set_flush_cb(disp, s_orig_flush);
    esp_log_level_set("*", prev);
    display_unlock();
    if (!s_shot_ok) {                       // the host stopped reading: say so, if it's back
        printf("ERR shot usb write timed out\n");
        return 1;
    }
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
    ui_update_now();
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
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    display_rot_t prev = display_get_rotation();
    if (display_set_rotation((display_rot_t)r) != ESP_OK) {
        display_unlock();
        printf("ERR rot %s not supported on this board\n", argv[1]);
        return 1;
    }
    if (settings_set("rot", argv[1]) != ESP_OK) {     // unsaved: keep the screen as it was
        display_set_rotation(prev);
        display_unlock();
        printf("ERR rot save\n");
        return 1;
    }
    if (lv_display_get_horizontal_resolution(disp) != w) ui_rebuild();   // wide <-> tall
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
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
    // the handshake: proto= tells the agent this board reads protocol v2 lines
    printf("fw=sparkesp32 version=%s idf=%s board=%s proto=%d\n", app->version, app->idf_ver, BOARD_ID,
           SPARK_PROTO);
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
        char b[8][16];
        printf("valid=%d age_ms=%ld src=%s host=%s cpu=%s gpu=%s mem=%s/%sMB temp=%s power_dw=%s "
               "cores=%d gpu_mhz=%s gpu_stuck=%s\n", s.valid, s.valid ? (long)age : -1L,
               spark_src_name(s.source), s.host[0] ? s.host : "-", fv(b[0], s.cpu), fv(b[1], s.gpu),
               fv(b[2], s.mem_used_mb), fv(b[3], s.mem_total_mb), fv(b[4], s.temp_c),
               fv(b[5], s.power_dw), s.ncores, fv(b[6], s.gpu_mhz), fv(b[7], s.gpu_stuck));
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
    rc.max_cmdline_length = 256;     // a 32-core v2 stat line with a 63-char host is ~210 chars
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

    // the board's sdkconfig.defaults picks the console: the chip's own USB, or a UART
    // behind a USB-UART bridge
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#elif CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#else
#error "SparkESP32 needs its console on USB-Serial/JTAG or a UART (the board's sdkconfig.defaults)"
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
