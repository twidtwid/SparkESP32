#include "status.h"

#include <errno.h>
#include <string.h>

// The screen's fonts are ASCII plus a few symbols: no typographic ellipsis.
static const char *problem_text(link_problem_t p)
{
    switch (p) {
    case LINK_PROBLEM_NO_NETWORK:   return "no network";
    case LINK_PROBLEM_BAD_PASSWORD: return "wrong password";
    case LINK_PROBLEM_CANT_JOIN:    return "can't join";
    default:                        return "joining...";
    }
}

static const char *feed_text(feed_t f)
{
    switch (f) {
    case FEED_AGENT_DOWN:  return "agent stopped";
    case FEED_UNREACHABLE: return "Spark unreachable";
    case FEED_STALE:       return "agent stale";
    case FEED_BAD:         return "bad reply";
    default:               return "";
    }
}

status_t status_of(const status_in_t *in)
{
    if (in->fresh)
        return (status_t){SCREEN_ONLINE, "ONLINE", in->source == SRC_WIFI ? "via WI-FI" : "via USB"};
    if (in->wifi_configured && in->link != LINK_CONNECTED)
        return (status_t){SCREEN_NO_WIFI, "NO WI-FI",
                          in->link == LINK_SCANNING ? "scanning" : problem_text(in->problem)};
    if (in->wifi_configured && !in->url_configured)
        return (status_t){SCREEN_SET_HOST, "SET HOST", ""};
    if (in->wifi_configured || in->ever_fed)   // over USB alone, the reason is unknowable
        return (status_t){SCREEN_OFFLINE, "OFFLINE", in->wifi_configured ? feed_text(in->feed) : ""};
    return (status_t){SCREEN_WAITING, "WAITING", ""};
}

bool status_redraw_due(const char *shown_label, const char *shown_reason, const char *label,
                       const char *reason, uint32_t since_ms, uint32_t min_ms)
{
    if (min_ms == 0 || !shown_label || !shown_reason) return true;
    if (strcmp(shown_label, label) != 0 || strcmp(shown_reason, reason) != 0) return true;
    return since_ms >= min_ms;
}

feed_t feed_classify(bool connected, int sock_errno, int http_status, bool parsed)
{
    if (!connected)   // refused or reset: the Spark is up and nothing listens on the port
        return sock_errno == ECONNREFUSED || sock_errno == ECONNRESET ? FEED_AGENT_DOWN : FEED_UNREACHABLE;
    if (http_status <= 0) return FEED_AGENT_DOWN;
    if (http_status == 503) return FEED_STALE;
    if (http_status != 200 || !parsed) return FEED_BAD;
    return FEED_OK;
}

const char *feed_name(feed_t f)
{
    static const char *const names[] = {"none", "ok", "agent-down", "unreachable", "stale", "bad"};
    return names[f];
}

const char *screen_name(screen_t s)
{
    static const char *const names[] = {"online", "no-wifi", "set-host", "offline", "waiting"};
    return names[s];
}
