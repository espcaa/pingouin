/*
 * Unit test for ui/pingouin-notch-park.h (the guest's pointer goes into the
 * hidden NOTCH output when the Mac's pointer leaves the VM up into
 * Omanotch's strip).
 *
 * First the decision and the place, case by case. Then a small model of the
 * hand-off at the strip's edge, millisecond by millisecond: QEMU hides and
 * shows the Mac's cursor at once (grab / ungrab), the guest draws its arrow
 * where its last tablet position put it, and notchcast hides or shows it
 * through Hyprland's cursor:invisible, which Hyprland applies only on a
 * periodic tick (85-540 ms measured on the MacBook Air; 300 ms here). The
 * model counts the cursors on screen: the Mac's arrow, plus the guest's if
 * any of it hangs into the VM's picture and it is not invisible. The old
 * way (no park, the guest hides) must show the bug (two arrows or none for
 * a long time); the new way (park, the guest never hides) one cursor at
 * every moment but the one frame the guest takes to draw.
 *
 * cc -Wall -Werror -I<qemu>/ui test-notch-park.c -o t && ./t
 */
#include <stdio.h>
#include <string.h>

#include "pingouin-notch-park.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static bool near(double a, double b)
{
    return a - b < 1e-9 && b - a < 1e-9;
}

/* The Air's layout under Pingouin.app: Virtual-1 1280x800, NOTCH 32 rows above it. */
static const PingouinParkBox SCREEN = { 0, 32, 1280, 800 };
static const PingouinParkBox NOTCH = { 0, 0, 1280, 32 };
static const PingouinParkBox ALL = { 0, 0, 1280, 832 };

static PingouinParkIn leaving_up(void)
{
    return (PingouinParkIn){ .absolute = true, .grabbed = true, .full_screen = true,
                           .notch_inset = 32, .exit_y = 800, .view_h = 800 };
}

static void test_wanted(void)
{
    PingouinParkIn in = leaving_up();

    CHECK(pingouin_park_wanted(&in), "up into the strip in full screen below a notch");
    in.exit_y = 799.2;
    CHECK(pingouin_park_wanted(&in), "the top row counts (AppKit reports it a little inside)");
    in = leaving_up();
    in.exit_y = 400;
    CHECK(!pingouin_park_wanted(&in), "out through a side: the pointer goes to another display");
    in = leaving_up();
    in.full_screen = false;
    CHECK(!pingouin_park_wanted(&in), "windowed: no strip above the window");
    in = leaving_up();
    in.notch_inset = 0;
    CHECK(!pingouin_park_wanted(&in), "a display without a notch (no strip)");
    in = leaving_up();
    in.grabbed = false;
    CHECK(!pingouin_park_wanted(&in), "the VM does not have the pointer: the guest gets nothing");
    in = leaving_up();
    in.absolute = false;
    CHECK(!pingouin_park_wanted(&in), "a relative pointer is captured, it never leaves");
    in = leaving_up();
    in.view_h = 0;
    CHECK(!pingouin_park_wanted(&in), "no view yet");
}

static void test_place(void)
{
    double nx = -1, ny = -1;
    PingouinParkBox none = { 0, 0, 0, 0 };

    CHECK(pingouin_park_place(SCREEN, NOTCH, ALL, &nx, &ny), "NOTCH right above");
    CHECK(near(nx, 0.5) && near(ny, 0),
          "the middle of NOTCH's top row, behind the camera (not the exit's x: notchcast "
          "keeps the strip under the guest's pointer from the last frame): %f %f", nx, ny);

    CHECK(!pingouin_park_place(SCREEN, none, ALL, &nx, &ny),
          "no NOTCH (Omanotch off, or the guest has not said yet)");
    CHECK(!pingouin_park_place(SCREEN, (PingouinParkBox){ 0, 32, 1280, 32 }, ALL, &nx, &ny),
          "NOTCH over the screen's top edge (another display above): its rows are the screen's");
    CHECK(!pingouin_park_place(SCREEN, (PingouinParkBox){ 0, -10, 1280, 32 }, ALL, &nx, &ny),
          "NOTCH not touching the screen");
    CHECK(!pingouin_park_place(SCREEN, (PingouinParkBox){ 1280, 0, 1280, 32 }, ALL, &nx, &ny),
          "NOTCH above another output, not this one");
    CHECK(pingouin_park_place(SCREEN, (PingouinParkBox){ 0, 0.4, 1280, 31.8 }, ALL, &nx, &ny),
          "half a px of slack for scaled sizes");
    CHECK(pingouin_park_place(SCREEN, (PingouinParkBox){ 0, 0, 500, 32 }, ALL, &nx, &ny) &&
          near(nx, 499.0 / 1280), "a NOTCH narrower than the screen: its last column: %f", nx);

    /* An external display left of the built-in one: the box around all moves. */
    {
        PingouinParkBox screen = { 1920, 280, 1280, 800 };
        PingouinParkBox notch = { 1920, 248, 1280, 32 };
        PingouinParkBox all = { 0, 0, 3200, 1080 };

        CHECK(pingouin_park_place(screen, notch, all, &nx, &ny), "beside an external display");
        CHECK(near(nx, (1920 + 640) / 3200.0) && near(ny, 248 / 1080.0),
              "NOTCH's top row in the whole box: %f %f", nx, ny);
        CHECK(!pingouin_park_place((PingouinParkBox){ 0, 0, 1920, 1080 }, notch, all, &nx, &ny),
              "the external display's view: NOTCH is not above it");
    }
}

/* ---- the hand-off, millisecond by millisecond ---- */

enum { CURSOR_H = 24, TICK_MS = 300, DRAW_MS = 16 };

typedef struct World {
    bool fix;               /* the new way: park + the guest never hides */
    /* Mac */
    bool on_vm;             /* the Mac's pointer is over the VM's picture */
    double mac_y;           /* its place, in guest rows (above SCREEN.y: the strip) */
    bool mac_hidden;        /* QEMU's [NSCursor hide] */
    /* guest */
    double tablet_y;        /* the last tablet position QEMU sent (guest rows) */
    double drawn_y;         /* where the guest's last frame shows its arrow */
    bool invisible;         /* Hyprland's cursor:invisible as applied */
    bool want_invisible;    /* as notchcast set it */
    int apply_at;           /* Hyprland's next tick that applies it (-1: none) */
} World;

static void notchcast_cursor(World *w, int now, bool visible)
{
    if (w->fix) {
        return;             /* PINGOUIN_NOTCHPOINTER=1: never hides, never moves */
    }
    w->want_invisible = !visible;
    if (w->apply_at < 0) {
        w->apply_at = now + TICK_MS;
    }
}

static void tablet(World *w, double y)
{
    w->tablet_y = y;
}

/* The pointer moves to row y (guest rows; < SCREEN.y is the strip). */
static void move(World *w, int now, double y)
{
    bool to_vm = y >= SCREEN.y;

    if (w->on_vm && !to_vm) {
        /* QEMU mouseExited: park (new), ungrab -> the Mac's arrow at once */
        PingouinParkIn in = leaving_up();
        double nx, ny;

        if (w->fix && pingouin_park_wanted(&in) &&
            pingouin_park_place(SCREEN, NOTCH, ALL, &nx, &ny)) {
            tablet(w, ny * ALL.h + ALL.y);
        }
        w->mac_hidden = false;
        notchcast_cursor(w, now, false);        /* Omanotch: strip entered */
    } else if (!w->on_vm && to_vm) {
        /* QEMU mouseEntered: grab -> hidden at once; Omanotch: strip left */
        w->mac_hidden = true;
        notchcast_cursor(w, now, true);
    }
    w->on_vm = to_vm;
    w->mac_y = y;
    if (to_vm) {
        tablet(w, y);                           /* mouseMoved */
    }
}

static void step(World *w, int now)
{
    if (now % DRAW_MS == 0) {
        w->drawn_y = w->tablet_y;           /* the guest's next frame */
    }
    if (w->apply_at >= 0 && now >= w->apply_at) {
        w->invisible = w->want_invisible;
        w->apply_at = -1;
    }
}

static int cursors(const World *w)
{
    bool guest = !w->invisible && w->drawn_y + CURSOR_H > SCREEN.y;

    return !w->mac_hidden + guest;
}

/* Up from the VM's top rows into the strip and back, `ms` apart, n times. */
static void crossings(bool fix, int ms, int *wrong, int *longest)
{
    World w = { .fix = fix, .on_vm = true, .mac_y = 300, .mac_hidden = true,
                .tablet_y = 300, .drawn_y = 300, .apply_at = -1 };
    int run = 0, now = 0;

    *wrong = *longest = 0;
    for (int n = 0; n < 10; n++) {
        for (int t = 0; t < 2 * ms; t++, now++) {
            if (t == 0) {
                move(&w, now, SCREEN.y + 2);    /* the VM's top row */
            } else if (t == ms / 2) {
                move(&w, now, 10);              /* on the strip */
            } else if (t == ms + ms / 2) {
                move(&w, now, SCREEN.y + 30);   /* back down into the VM */
            }
            step(&w, now);
            if (cursors(&w) != 1) {
                (*wrong)++;
                run++;
                *longest = run > *longest ? run : *longest;
            } else {
                run = 0;
            }
        }
    }
}

static void test_handoff(void)
{
    int wrong, longest;

    crossings(false, 600, &wrong, &longest);
    CHECK(longest >= 100, "old way: two arrows or none for >= 100 ms (Hyprland's tick): %d ms",
          longest);
    crossings(true, 600, &wrong, &longest);
    CHECK(longest <= DRAW_MS, "new way, slow: at most one frame out of step: %d ms", longest);
    crossings(true, 60, &wrong, &longest);
    CHECK(longest <= DRAW_MS, "new way, fast: at most one frame out of step: %d ms", longest);
    crossings(true, 20, &wrong, &longest);
    CHECK(longest <= DRAW_MS, "new way, faster than a frame: %d ms", longest);
}

int main(void)
{
    test_wanted();
    test_place();
    test_handoff();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures != 0;
}
