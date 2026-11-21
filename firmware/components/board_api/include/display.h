#pragma once
// The display half of the board contract (see board.h): LVGL on the board's panel,
// one lock around it, and the screen rotation. Each board implements these in its
// own display.c; main/ draws with plain LVGL on the default display.

#include <string.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DISPLAY_ROT_NONE = 0, DISPLAY_ROT_CCW = 1, DISPLAY_ROT_CW = 2 } display_rot_t;

// Start LVGL on the panel, after board_init(). A rotation the board can't do
// falls back to its default.
esp_err_t display_start(display_rot_t rot);

// LVGL is not thread-safe: hold this around every lv_* call from any task.
// timeout_ms < 0 waits forever. Recursive.
esp_err_t display_lock(int timeout_ms);
void      display_unlock(void);

// Caller holds the lock. ESP_ERR_NOT_SUPPORTED if this board can't show that
// rotation (e.g. a landscape-only layout asked for portrait).
esp_err_t     display_set_rotation(display_rot_t rot);
display_rot_t display_get_rotation(void);

// "none" | "ccw" | "cw", as saved in NVS and typed at the console
static inline int display_rot_from_name(const char *name)
{
    static const char *const names[] = {"none", "ccw", "cw"};
    for (int i = 0; i < 3; i++)
        if (!strcmp(name, names[i])) return i;
    return -1;
}

static inline const char *display_rot_name(display_rot_t rot)
{
    static const char *const names[] = {"none", "ccw", "cw"};
    return names[rot];
}

#ifdef __cplusplus
}
#endif
