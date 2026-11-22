// Host tests for the firmware's two state machines: the Wi-Fi link supervisor
// (firmware/main/link.c) and what the screen says (firmware/main/status.c).
// Built and run by `make check` with the system C compiler; no ESP-IDF needed.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "link.h"
#include "status.h"

static int failures, total;

#define CHECK(cond, ...) do { total++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static link_t fresh_link(int nssids, uint32_t now)
{
    link_t l = {.nssids = nssids};
    link_reset(&l, now);
    return l;
}

// ---------------------------------------------------------------- link.c

static void test_reset(void)
{
    link_t l = fresh_link(0, 1000);
    CHECK(l.state == LINK_UNCONFIGURED, "no SSIDs -> unconfigured, got %s", link_state_name(l.state));
    CHECK(!link_tick(&l, 999999), "unconfigured never joins");

    l = fresh_link(2, 1000);
    CHECK(l.state == LINK_BACKOFF && l.next_try_ms == 1000, "SSIDs -> backoff, due now");
    CHECK(link_tick(&l, 1000) && l.state == LINK_JOINING && l.cur == 0, "first tick joins SSID 0");
}

static void test_join_and_drop(void)
{
    link_t l = fresh_link(1, 0);
    link_tick(&l, 0);
    link_joined(&l, 500);
    CHECK(l.state == LINK_CONNECTED && l.fails == 0 && l.backoff_ms == LINK_BACKOFF_MIN_MS, "joined");
    CHECK(!link_tick(&l, 60000), "connected: the tick does nothing");

    link_disconnected(&l, 200, 70000);           // beacon timeout: the AP went away
    CHECK(l.state == LINK_BACKOFF && l.next_try_ms == 70000, "a drop retries now");
    CHECK(link_problem(&l) == LINK_PROBLEM_NONE, "a drop isn't a join problem");
    CHECK(link_tick(&l, 70000) && l.state == LINK_JOINING, "rejoins on the next tick");
}

static void test_backoff_doubles_to_a_cap(void)
{
    link_t l = fresh_link(1, 0);
    uint32_t now = 0;
    const uint32_t want[] = {2000, 4000, 8000, 16000, 30000, 30000};
    for (int i = 0; i < 6; i++) {
        CHECK(link_tick(&l, now), "attempt %d starts when due", i);
        link_disconnected(&l, 201, now + 100);   // no AP found
        uint32_t wait = l.next_try_ms - (now + 100);
        CHECK(wait == want[i], "wait after failure %d = %u, want %u", i + 1, (unsigned)wait, (unsigned)want[i]);
        CHECK(!link_tick(&l, l.next_try_ms - 1), "not before it's due");
        now = l.next_try_ms;
    }
    CHECK(link_problem(&l) == LINK_PROBLEM_NO_NETWORK, "201 -> no network");
    link_tick(&l, now);
    link_joined(&l, now + 50);
    CHECK(l.backoff_ms == LINK_BACKOFF_MIN_MS && link_problem(&l) == LINK_PROBLEM_NONE, "joining resets");
}

static void test_rotation(void)
{
    link_t l = fresh_link(2, 0);
    uint32_t now = 0;
    for (int i = 0; i < LINK_FAILS_TO_ROTATE; i++) {
        CHECK(link_tick(&l, now) && l.cur == 0, "fail %d stays on SSID 0", i + 1);
        link_disconnected(&l, 15, now);          // 4-way handshake timeout
        now = l.next_try_ms;
    }
    CHECK(link_problem(&l) == LINK_PROBLEM_BAD_PASSWORD, "15 -> wrong password");
    CHECK(link_tick(&l, now) && l.cur == 1 && l.fails == 0, "after 3 fails, SSID 1");
    for (int i = 0; i < LINK_FAILS_TO_ROTATE; i++) {
        link_disconnected(&l, 202, now);
        now = l.next_try_ms;
        link_tick(&l, now);
    }
    CHECK(l.cur == 0, "and around the ring back to SSID 0, got %d", l.cur);

    link_t one = fresh_link(1, 0);
    now = 0;
    for (int i = 0; i < 5; i++) {
        link_tick(&one, now);
        link_disconnected(&one, 15, now);
        now = one.next_try_ms;
    }
    link_tick(&one, now);
    CHECK(one.cur == 0, "one SSID: nothing to rotate to");
}

static void test_attempt_timeout(void)
{
    link_t l = fresh_link(1, 0);
    link_tick(&l, 0);
    CHECK(!link_tick(&l, LINK_ATTEMPT_MS - 1) && l.state == LINK_JOINING, "still joining at 19.999 s");
    link_tick(&l, LINK_ATTEMPT_MS);
    CHECK(l.state == LINK_BACKOFF && l.fails == 1, "a silent join times out as a failure");
    CHECK(l.next_try_ms == LINK_ATTEMPT_MS + LINK_BACKOFF_MIN_MS, "and backs off from then");

    link_t m = fresh_link(1, 0);
    link_tick(&m, 0);
    link_join_not_started(&m, 10);
    CHECK(m.state == LINK_BACKOFF && m.fails == 1, "a join the driver refused counts as failed");
}

static void test_events_outside_joining_are_ignored(void)
{
    link_t l = fresh_link(1, 0);                 // BACKOFF: e.g. our own disconnect after a reset
    link_disconnected(&l, 8, 5);
    CHECK(l.state == LINK_BACKOFF && l.fails == 0 && l.next_try_ms == 0, "backoff ignores a disconnect");

    link_scan(&l, 10);
    CHECK(l.state == LINK_SCANNING && !link_tick(&l, 99999), "scanning: the supervisor keeps off");
    link_disconnected(&l, 8, 20);
    CHECK(l.state == LINK_SCANNING, "scanning ignores the disconnect it caused");
    link_reset(&l, 3000);
    CHECK(l.state == LINK_BACKOFF && link_tick(&l, 3000), "scan done: rejoin at once");

    link_t u = fresh_link(0, 0);
    link_disconnected(&u, 8, 5);
    CHECK(u.state == LINK_UNCONFIGURED, "unconfigured ignores a disconnect");
}

// The wedge this machine was made explicit for: a deadline left over from the
// last join, 2^31 ms (~24.9 days) stale, read as "in the future" after a drop.
static void test_drop_after_25_days_rejoins_at_once(void)
{
    link_t l = fresh_link(1, 1000);
    link_tick(&l, 1000);
    link_joined(&l, 1500);
    uint32_t later = 1500u + 2147483648u + 5000u;   // more than 2^31 ms connected
    link_disconnected(&l, 200, later);
    CHECK(link_tick(&l, later) && l.state == LINK_JOINING, "rejoins at once after weeks connected");
}

static void test_backoff_across_the_clock_wrap(void)
{
    uint32_t near_wrap = 0xFFFFFFFFu - 1000u;      // the ms clock wraps every ~49.7 days
    link_t l = fresh_link(1, near_wrap);
    link_tick(&l, near_wrap);
    link_disconnected(&l, 201, near_wrap);         // retry 2 s later: past the wrap
    CHECK(!link_tick(&l, near_wrap + 1999u), "not due 1 ms before, across the wrap");
    CHECK(link_tick(&l, near_wrap + 2000u), "due on time across the wrap");
}

static void test_problem_mapping(void)
{
    link_t l = {0};
    const struct { int reason; link_problem_t want; } cases[] = {
        {0, LINK_PROBLEM_NONE}, {201, LINK_PROBLEM_NO_NETWORK}, {212, LINK_PROBLEM_NO_NETWORK},
        {15, LINK_PROBLEM_BAD_PASSWORD}, {202, LINK_PROBLEM_BAD_PASSWORD},
        {204, LINK_PROBLEM_BAD_PASSWORD}, {205, LINK_PROBLEM_CANT_JOIN}, {211, LINK_PROBLEM_CANT_JOIN},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        l.reason = cases[i].reason;
        CHECK(link_problem(&l) == cases[i].want, "reason %d", cases[i].reason);
    }
    CHECK(strcmp(link_state_name(LINK_SCANNING), "scanning") == 0, "state names");
}

// ---------------------------------------------------------------- status.c

static status_in_t wifi_up(void)   // Wi-Fi joined, URL set, no fresh sample
{
    return (status_in_t){.ever_fed = true, .source = SRC_WIFI, .wifi_configured = true,
                         .url_configured = true, .link = LINK_CONNECTED};
}

static void expect(status_in_t in, screen_t screen, const char *label, const char *reason, const char *what)
{
    status_t st = status_of(&in);
    CHECK(st.screen == screen && strcmp(st.label, label) == 0 && strcmp(st.reason, reason) == 0,
          "%s: got %s / '%s'", what, st.label, st.reason);
}

static void test_screen(void)
{
    status_in_t in = wifi_up();
    in.fresh = true;
    expect(in, SCREEN_ONLINE, "ONLINE", "via WI-FI", "fresh over Wi-Fi");
    in.source = SRC_USB;
    expect(in, SCREEN_ONLINE, "ONLINE", "via USB", "fresh over USB beats a dead link");
    in.link = LINK_BACKOFF;
    expect(in, SCREEN_ONLINE, "ONLINE", "via USB", "fresh over USB, Wi-Fi down");

    in = wifi_up();
    in.link = LINK_JOINING;
    expect(in, SCREEN_NO_WIFI, "NO WI-FI", "joining...", "first join");
    in.link = LINK_BACKOFF, in.problem = LINK_PROBLEM_BAD_PASSWORD;
    expect(in, SCREEN_NO_WIFI, "NO WI-FI", "wrong password", "bad password");
    in.problem = LINK_PROBLEM_NO_NETWORK;
    expect(in, SCREEN_NO_WIFI, "NO WI-FI", "no network", "no network");
    in.problem = LINK_PROBLEM_CANT_JOIN;
    expect(in, SCREEN_NO_WIFI, "NO WI-FI", "can't join", "other refusal");
    in.link = LINK_SCANNING;
    expect(in, SCREEN_NO_WIFI, "NO WI-FI", "scanning", "during wifi scan");

    in = wifi_up();
    in.url_configured = false;
    expect(in, SCREEN_SET_HOST, "SET HOST", "", "joined, no URL");

    in = wifi_up();
    const struct { feed_t feed; const char *reason; } feeds[] = {
        {FEED_AGENT_DOWN, "agent stopped"}, {FEED_UNREACHABLE, "Spark unreachable"},
        {FEED_STALE, "agent stale"}, {FEED_BAD, "bad reply"}, {FEED_NONE, ""}, {FEED_OK, ""},
    };
    for (size_t i = 0; i < sizeof feeds / sizeof feeds[0]; i++) {
        in.feed = feeds[i].feed;
        expect(in, SCREEN_OFFLINE, "OFFLINE", feeds[i].reason, feed_name(feeds[i].feed));
    }

    in = (status_in_t){.ever_fed = true, .source = SRC_USB, .feed = FEED_AGENT_DOWN};
    expect(in, SCREEN_OFFLINE, "OFFLINE", "", "USB only: the cause is unknowable");
    expect((status_in_t){0}, SCREEN_WAITING, "WAITING", "", "nothing yet");
    CHECK(strcmp(screen_name(SCREEN_NO_WIFI), "no-wifi") == 0, "screen names");
}

static void test_feed_classify(void)
{
    CHECK(feed_classify(false, ECONNREFUSED, 0, false) == FEED_AGENT_DOWN, "refused: agent down");
    CHECK(feed_classify(false, ECONNRESET, 0, false) == FEED_AGENT_DOWN, "reset: agent down");
    CHECK(feed_classify(false, ETIMEDOUT, 0, false) == FEED_UNREACHABLE, "timeout: unreachable");
    CHECK(feed_classify(false, 0, 0, false) == FEED_UNREACHABLE, "unresolved name: unreachable");
    CHECK(feed_classify(true, 0, 0, false) == FEED_AGENT_DOWN, "connected, silent: agent down");
    CHECK(feed_classify(true, 0, 503, false) == FEED_STALE, "503: stale");
    CHECK(feed_classify(true, 0, 404, false) == FEED_BAD, "404: bad");
    CHECK(feed_classify(true, 0, 200, false) == FEED_BAD, "200, unparseable: bad");
    CHECK(feed_classify(true, 0, 200, true) == FEED_OK, "200, parsed: ok");
}

// ---------------------------------------------------------------- the e-paper's cadence

static void test_redraw_due(void)
{
    CHECK(status_redraw_due("ONLINE", "via USB", "ONLINE", "via USB", 0, 0),
          "min 0 (every screen but e-paper): always");
    CHECK(status_redraw_due(NULL, NULL, "WAITING", "", 0, 10000), "the first redraw: always");
    CHECK(!status_redraw_due("ONLINE", "via USB", "ONLINE", "via USB", 9999, 10000),
          "same header, too soon: wait");
    CHECK(status_redraw_due("ONLINE", "via USB", "ONLINE", "via USB", 10000, 10000), "due");
    CHECK(status_redraw_due("ONLINE", "via USB", "OFFLINE", "", 1, 10000), "a new state: at once");
    CHECK(status_redraw_due("ONLINE", "via USB", "ONLINE", "CLOCK STUCK", 1, 10000),
          "a new line under the state: at once");
    CHECK(status_redraw_due("ONLINE", "via USB", "ONLINE", "via WI-FI", 1, 10000),
          "USB to Wi-Fi: at once");
    char copy[] = "ONLINE";                 // compared by text, not by pointer
    CHECK(!status_redraw_due(copy, "via USB", "ONLINE", "via USB", 1, 10000), "equal text: wait");
}

int main(void)
{
    test_reset();
    test_join_and_drop();
    test_backoff_doubles_to_a_cap();
    test_rotation();
    test_attempt_timeout();
    test_events_outside_joining_are_ignored();
    test_drop_after_25_days_rejoins_at_once();
    test_backoff_across_the_clock_wrap();
    test_problem_mapping();
    test_screen();
    test_feed_classify();
    test_redraw_due();
    if (failures) {
        printf("%d of %d state checks FAILED\n", failures, total);
        return 1;
    }
    printf("%d/%d state checks passed\n", total, total);
    return 0;
}
