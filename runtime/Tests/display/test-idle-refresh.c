/* Test for qemu-cocoa-idle-refresh.patch: the refresh tick's rate, taken from
 * the patched ui/cocoa.m by the build (the block from COCOA_REFRESH_SLOW_MS to
 * the end of cocoa_refresh_tick()), against stand-ins for QEMU's console API
 * and clock. No GL, no window, no VM.
 *
 *   awk '/^#define COCOA_REFRESH_SLOW_MS/{f=1} f{print}
 *        f&&/^static void cocoa_refresh_tick\(bool pending\)$/{t=1}
 *        t&&/^}$/{exit}' ui/cocoa.m > DIR/idle-refresh.inc
 *   cc -IDIR test-idle-refresh.c -o t && ./t && PINGOUIN_IDLE_REFRESH=0 ./t off
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define GUI_REFRESH_INTERVAL_DEFAULT 30
#define PINGOUIN_MAX_OUTPUTS 4
#define QEMU_CLOCK_REALTIME 0

typedef struct DisplayChangeListener {
    void *ds;                       /* registered when set */
    uint64_t update_interval;
} DisplayChangeListener;

typedef struct PingouinHead {
    DisplayChangeListener dcl;
} PingouinHead;

static DisplayChangeListener dcl;
static PingouinHead pingouin_heads[PINGOUIN_MAX_OUTPUTS];
static int pingouin_outputs = 3;
static bool display_opengl = true;
static int64_t now_ms;
static int set_calls;

static int64_t qemu_clock_get_ms(int clock)
{
    (void)clock;
    return now_ms;
}

static void qemu_console_listener_set_refresh(DisplayChangeListener *l, uint64_t ms)
{
    l->update_interval = ms;
    set_calls++;
}

static void info_report(const char *fmt, ...)
{
    (void)fmt;
}

static void cocoa_refresh_set(DisplayChangeListener *l, int output, uint64_t ms);
static void cocoa_refresh_activity(void);

#include "idle-refresh.inc"

static int failures;

static void expect(const char *what, uint64_t want0, uint64_t want1)
{
    uint64_t got0 = dcl.update_interval, got1 = pingouin_heads[1].dcl.update_interval;
    if (got0 != want0 || got1 != want1) {
        printf("FAIL %s: want %llu/%llu ms, got %llu/%llu ms\n", what,
               (unsigned long long)want0, (unsigned long long)want1,
               (unsigned long long)got0, (unsigned long long)got1);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

/* Ticks every `step` ms from now_ms up to `until`. */
static void ticks(int64_t until, int step, bool pending)
{
    while (now_ms < until) {
        now_ms += step;
        cocoa_refresh_tick(pending);
    }
}

int main(int argc, char **argv)
{
    bool off = argc > 1 && !strcmp(argv[1], "off");
    static int registered;

    dcl.ds = &registered;
    pingouin_heads[1].dcl.ds = &registered;
    /* pingouin_heads[2] is not registered: never touched. */

    cocoa_refresh_set(&dcl, 0, 8);                  /* a 120 Hz main window */
    cocoa_refresh_set(&pingouin_heads[1].dcl, 1, 16); /* a 60 Hz extra output */
    expect("the display's rate at start", 8, 16);

    if (off) {
        ticks(now_ms + 5000, 8, false);
        expect("PINGOUIN_IDLE_REFRESH=0: the display's rate after 5 s idle", 8, 16);
        return failures ? 1 : 0;
    }

    ticks(now_ms + 900, 8, false);
    expect("under a second without work: still fast", 8, 16);
    ticks(now_ms + 200, 8, false);
    expect("a second without work: slow", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);
    ticks(now_ms + 3 * COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS, false);
    expect("stays slow while idle", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);

    cocoa_refresh_activity();                       /* a 2D update, say */
    expect("work: the display's rate at once", 8, 16);
    ticks(now_ms + 900, 8, false);
    expect("under a second after the work: fast", 8, 16);
    ticks(now_ms + 200, 8, false);
    expect("a second after the work: slow again", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);

    ticks(now_ms + COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS, true);
    expect("a frame not shown yet: fast", 8, 16);
    ticks(now_ms + 3000, 8, true);
    expect("still fast while it is pending", 8, 16);
    ticks(now_ms + 900, 8, false);
    expect("shown: fast for under a second", 8, 16);
    ticks(now_ms + 200, 8, false);
    expect("then slow", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);

    cocoa_refresh_set(&dcl, 0, 16);                 /* window moved to a 60 Hz display */
    expect("a new display rate while slow stays slow", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);
    cocoa_refresh_activity();
    expect("then the new rate", 16, 16);

    cocoa_refresh_set(&dcl, 0, 0);                  /* rate unknown: QEMU's default */
    ticks(now_ms + 1200, 30, false);
    expect("QEMU's default rate slows too", COCOA_REFRESH_SLOW_MS, COCOA_REFRESH_SLOW_MS);
    cocoa_refresh_activity();
    expect("and comes back as the default (0)", 0, 16);

    if (pingouin_heads[2].dcl.update_interval != 0) {
        printf("FAIL an unregistered output was changed\n");
        failures++;
    } else {
        printf("ok   an unregistered output is left alone\n");
    }
    return failures ? 1 : 0;
}
