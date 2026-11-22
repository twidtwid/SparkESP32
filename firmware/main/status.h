#pragma once
// What the screen's header says, decided in one place: one of five states plus
// a reason line. Plain C (no LVGL, no ESP-IDF), so the host tests cover every
// case; ui.c only draws the result.
//
//   ONLINE    a fresh sample        reason: via USB / via WI-FI
//   NO WI-FI  Wi-Fi not joined      reason: joining... / no network / wrong password / can't join
//   SET HOST  joined, no agent URL
//   OFFLINE   no fresh sample       reason (Wi-Fi): agent stopped / Spark unreachable / agent stale / bad reply
//   WAITING   nothing configured or received yet

#include <stdbool.h>
#include <stdint.h>
#include "link.h"
#include "spark_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {              // how the last /stat poll over Wi-Fi went
    FEED_NONE,              // no poll since joining (or USB is feeding)
    FEED_OK,
    FEED_AGENT_DOWN,        // the Spark refused the connection, or took it and never answered
    FEED_UNREACHABLE,       // no answer at all, or its name didn't resolve
    FEED_STALE,             // HTTP 503: the agent is up but its samples are old
    FEED_BAD,               // any other HTTP status, or a line the parser rejected
} feed_t;

typedef enum { SCREEN_ONLINE, SCREEN_NO_WIFI, SCREEN_SET_HOST, SCREEN_OFFLINE, SCREEN_WAITING } screen_t;

typedef struct {
    bool fresh;                 // the last sample is younger than SPARK_STALE_MS
    bool ever_fed;              // any sample since boot
    spark_src_t source;         // where the last sample came from
    bool wifi_configured, url_configured;
    link_state_t link;
    link_problem_t problem;
    feed_t feed;
} status_in_t;

typedef struct {
    screen_t screen;
    const char *label;          // "ONLINE", "NO WI-FI", ...
    const char *reason;         // the second line, or ""
} status_t;

status_t status_of(const status_in_t *in);

// One poll's outcome. connected: the TCP connection was made; sock_errno: why
// not, if it wasn't; http_status: 0 if no response came; parsed: the body was a
// stat line the parser accepted.
feed_t feed_classify(bool connected, int sock_errno, int http_status, bool parsed);

// Whether the screen should take new readings now. A slow screen (e-paper: the
// board's BOARD_UI_MIN_REFRESH_MS) shows new readings at most every min_ms, but a
// change in what the header says (its label, or the line under it) goes out at
// once. shown_label/shown_reason: what the header said at the last redraw (NULL
// before the first). min_ms 0: every update redraws, as on every other screen.
bool status_redraw_due(const char *shown_label, const char *shown_reason, const char *label,
                       const char *reason, uint32_t since_ms, uint32_t min_ms);

const char *feed_name(feed_t f);        // lowercase, for `status`
const char *screen_name(screen_t s);    // lowercase, for `status`

#ifdef __cplusplus
}
#endif
