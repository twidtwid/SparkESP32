#pragma once
// All functions here must be called with display_lock() held.
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_TITLE_MAX 24             // header title, printable ASCII (saved as "name")

void ui_init(const char *name);     // build the static UI; name = saved header title
void ui_update(void);               // repaint from g_spark + staleness (at the board's cadence)
void ui_update_now(void);           // the same, at once: for bench commands (demo, shot)
void ui_set_name(const char *name); // NULL/"" -> the Spark's hostname from the feed
void ui_rebuild(void);              // after the canvas changed shape (a rotation): lay it all out again
status_t ui_status(void);           // what the header says right now (see status.h)

#ifdef __cplusplus
}
#endif
