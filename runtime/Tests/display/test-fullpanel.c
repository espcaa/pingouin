/*
 * Unit test for ui/pingouin-fullpanel.h (FullPanel: the VM's full-screen
 * window over the whole notched display).
 *
 * No notched Mac is needed: the displays are made up from the sizes macOS
 * reports (points, bottom-left origin): MacBook Air 13" 1470x956 and 15"
 * 1710x1112, MacBook Pro 14" 1512x982 and 16" 1728x1117 (a top inset
 * around 32-38), and external displays without an inset. Then which full
 * screens fall back and why, whether AppKit's reservation may be answered,
 * whether AppKit's layout counts (Split View does not), a window moving
 * between displays, the guest's size over the whole panel, the menu bar's
 * reveal at the top edge (only without "Keep the Dock away"), the app's
 * presentation options, and the Mission Control return checks.
 *
 * cc -Wall -Werror -I<qemu>/ui test-fullpanel.c -o t && ./t
 */
#include <stdio.h>

#include "pingouin-fullpanel.h"
#include "pingouin-clean-size.h"

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

typedef struct {
    const char *name;
    PingouinFPRect frame;
    double notch;
} Display;

static const Display builtins[] = {
    { "MacBook Air 13", { 0, 0, 1470, 956 }, 32 },
    { "MacBook Air 15", { 0, 0, 1710, 1112 }, 33 },
    { "MacBook Pro 14", { 0, 0, 1512, 982 }, 32 },
    { "MacBook Pro 16", { 0, 0, 1728, 1117 }, 38 },
    /* A built-in that is not the main display: placed right of an external. */
    { "MacBook Air 13 beside", { 2560, -300, 1470, 956 }, 32 },
};

static const Display externals[] = {
    { "LG 5K", { 0, 0, 2560, 1440 }, 0 },
    { "Studio Display left", { -2560, 0, 2560, 1440 }, 0 },
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

static PingouinFPRect below_notch(const Display *d)
{
    PingouinFPRect r = d->frame;
    r.h -= d->notch;
    return r;
}

/* Every part there, the menu bar setting as macOS's default. */
static PingouinFPFallback fallback(const Display *d)
{
    return pingouin_fp_fallback(d->notch, false, true, true);
}

static void test_requested(void)
{
    CHECK(pingouin_fp_requested("1"), "PINGOUIN_FULLPANEL=1 asks for it");
    CHECK(!pingouin_fp_requested(NULL), "unset: native");
    CHECK(!pingouin_fp_requested("0"), "0: native");
    CHECK(!pingouin_fp_requested("yes"), "only 1 counts");
    CHECK(!pingouin_fp_requested(""), "empty: native");
}

static void test_fallback(void)
{
    for (size_t i = 0; i < N(builtins); i++) {
        const Display *d = &builtins[i];
        CHECK(fallback(d) == PINGOUIN_FP_OK, "%s: FullPanel", d->name);
        CHECK(pingouin_fp_fallback(d->notch, true, true, true) == PINGOUIN_FP_MENU_BAR_SHOWN,
              "%s: menu bar always shown in full screen: native", d->name);
        CHECK(pingouin_fp_fallback(d->notch, false, false, true) == PINGOUIN_FP_NO_RESERVATION,
              "%s: no reservation: native", d->name);
        CHECK(pingouin_fp_fallback(d->notch, false, true, false) == PINGOUIN_FP_NO_SKYLIGHT,
              "%s: no SkyLight: native", d->name);
    }
    for (size_t i = 0; i < N(externals); i++) {
        CHECK(fallback(&externals[i]) == PINGOUIN_FP_NO_NOTCH,
              "%s: no camera housing, never", externals[i].name);
    }
    /* First reason first: no notch, the menu bar, AppKit, SkyLight. */
    CHECK(pingouin_fp_fallback(0, true, false, false) == PINGOUIN_FP_NO_NOTCH, "no notch first");
    CHECK(pingouin_fp_fallback(32, true, false, false) == PINGOUIN_FP_MENU_BAR_SHOWN, "then the menu bar");
    CHECK(pingouin_fp_fallback(32, false, false, false) == PINGOUIN_FP_NO_RESERVATION, "then AppKit");
    for (int f = PINGOUIN_FP_OK; f <= PINGOUIN_FP_NO_SKYLIGHT; f++) {
        CHECK(pingouin_fp_fallback_text((PingouinFPFallback)f)[0], "reason %d has a text", f);
    }
    /*
     * The line pingouin check reads: every fallback, but an external display's
     * window while the built-in display is on (it came after "strip covered").
     */
    CHECK(!pingouin_fp_fallback_checked(PINGOUIN_FP_OK, false), "no fallback, no line");
    CHECK(!pingouin_fp_fallback_checked(fallback(&externals[0]), true),
          "external display beside the open MacBook: its own line");
    CHECK(pingouin_fp_fallback_checked(fallback(&externals[0]), false),
          "external display only (lid closed, a Mac mini): the line check reads");
    for (int f = PINGOUIN_FP_MENU_BAR_SHOWN; f <= PINGOUIN_FP_NO_SKYLIGHT; f++) {
        CHECK(pingouin_fp_fallback_checked((PingouinFPFallback)f, true) &&
              pingouin_fp_fallback_checked((PingouinFPFallback)f, false), "reason %d: check reads it", f);
    }
}

/*
 * "Always show the menu bar in full screen" (#356): in the way only without
 * "Keep the Dock away" (the MacBook Air and the mini, 2026-10-12: with it,
 * the menu bar stays hidden in full screen whatever that setting says).
 */
static void test_menu_bar_setting(void)
{
    for (size_t i = 0; i < N(builtins); i++) {
        const Display *d = &builtins[i];
        CHECK(pingouin_fp_fallback(d->notch, pingouin_fp_menu_bar_in_the_way(true, true), true, true) ==
              PINGOUIN_FP_OK, "%s: menu bar always shown, Keep the Dock away on: FullPanel", d->name);
        CHECK(pingouin_fp_fallback(d->notch, pingouin_fp_menu_bar_in_the_way(true, false), true, true) ==
              PINGOUIN_FP_MENU_BAR_SHOWN,
              "%s: menu bar always shown, Keep the Dock away off: normal full screen", d->name);
        CHECK(!pingouin_fp_menu_bar_in_the_way(false, false) && !pingouin_fp_menu_bar_in_the_way(false, true),
              "%s: menu bar hidden in full screen (macOS's default): FullPanel either way", d->name);
    }
}

/* AppKit's reservation method: only AppKit's own is answered, nobody else's change. */
static void test_hook(void)
{
    CHECK(pingouin_fp_hook(true, true, true, false, false) == PINGOUIN_FP_HOOK_USABLE,
          "AppKit's own method, not installed yet: usable");
    CHECK(pingouin_fp_hook(true, true, false, true, true) == PINGOUIN_FP_HOOK_USABLE,
          "installed and still ours: usable");
    CHECK(pingouin_fp_hook(false, true, true, false, false) == PINGOUIN_FP_HOOK_MISSING,
          "class, selector or encoding changed: missing");
    CHECK(pingouin_fp_hook(true, false, true, false, false) == PINGOUIN_FP_HOOK_INHERITED,
          "inherited from a superclass: never changed (other classes would change)");
    CHECK(pingouin_fp_hook(true, true, false, false, false) == PINGOUIN_FP_HOOK_INTERCEPTED,
          "other code replaced it first: left alone");
    CHECK(pingouin_fp_hook(true, true, false, true, false) == PINGOUIN_FP_HOOK_REPLACED,
          "other code replaced ours: the next full screens are normal");
    for (int h = PINGOUIN_FP_HOOK_USABLE; h <= PINGOUIN_FP_HOOK_REPLACED; h++) {
        CHECK(pingouin_fp_hook_text((PingouinFPHook)h)[0], "hook state %d has a text", h);
    }
}

/* Whether AppKit's layout under the reservation answer counts. */
static void test_verdict(void)
{
    for (size_t i = 0; i < N(builtins); i++) {
        const Display *d = &builtins[i];
        PingouinFPRect half = d->frame;
        bool covers;

        half.w = d->frame.w / 2;

        /* During the way in: counted as soon as it covers, else not yet. */
        covers = pingouin_fp_rect_equal(d->frame, d->frame);
        CHECK(pingouin_fp_verdict(true, true, covers, true, false) == PINGOUIN_FP_ACCEPTED,
              "%s: covers the display: accepted", d->name);
        covers = pingouin_fp_rect_equal(below_notch(d), d->frame);
        CHECK(pingouin_fp_verdict(true, true, covers, true, false) == PINGOUIN_FP_PENDING,
              "%s: below the notch during the way in: not yet", d->name);
        CHECK(pingouin_fp_verdict(true, false, true, true, true) == PINGOUIN_FP_PENDING,
              "%s: not in full screen (yet): nothing decided", d->name);

        /* At didEnterFullScreen: final. */
        covers = pingouin_fp_rect_equal(half, d->frame);
        CHECK(pingouin_fp_verdict(true, true, covers, true, true) == PINGOUIN_FP_REJECTED,
              "%s: Split View's tile at didEnterFullScreen: AppKit's layout", d->name);
        covers = pingouin_fp_rect_equal(below_notch(d), d->frame);
        CHECK(pingouin_fp_verdict(true, true, covers, true, true) == PINGOUIN_FP_REJECTED,
              "%s: kept below the notch: AppKit's layout", d->name);
        CHECK(pingouin_fp_verdict(false, true, true, true, true) == PINGOUIN_FP_PENDING,
              "%s: not claiming (native full screen): nothing to decide", d->name);
    }
    CHECK(pingouin_fp_verdict(true, true, true, false, true) == PINGOUIN_FP_REJECTED,
          "covers an external display: never the strip's");
}

static void test_lost(void)
{
    CHECK(!pingouin_fp_lost(true, true, true, true), "covering: not lost");
    CHECK(pingouin_fp_lost(true, true, false, true), "display the same, window moved: lost");
    CHECK(!pingouin_fp_lost(true, true, false, false),
          "the display changed (a new resolution): the window follows, not lost");
    CHECK(!pingouin_fp_lost(true, false, false, true), "windowed: not a loss");
    CHECK(!pingouin_fp_lost(false, true, false, true), "never covered: nothing to lose");
}

/*
 * A window moving between displays (the displays patch makes one window per
 * display; a display plugged in or out moves them, Mission Control can move
 * a Space): each display decides for itself, and a full screen moved away
 * from the notch is normal there, then the strip's again.
 */
static void test_moves(void)
{
    const Display *path[] = { &builtins[0], &externals[0], &builtins[4], &externals[1], &builtins[0] };
    bool released = false;

    for (size_t i = 0; i < N(path); i++) {
        const Display *d = path[i];
        PingouinFPFallback want = d->notch > 0 ? PINGOUIN_FP_OK : PINGOUIN_FP_NO_NOTCH;
        CHECK(fallback(d) == want, "entry %zu on %s", i, d->name);
    }
    for (size_t i = 0; i < N(path); i++) {
        bool notch = path[i]->notch > 0;
        if (pingouin_fp_screen_releases(true, true, notch, released)) {
            released = true;
        } else if (pingouin_fp_screen_resumes(released, true, notch)) {
            released = false;
        }
        CHECK(released == !notch, "move %zu to %s: %s", i, path[i]->name,
              notch ? "the strip" : "normal full screen");
    }
    CHECK(!pingouin_fp_screen_releases(true, false, false, false), "windowed: nothing to release");
    CHECK(!pingouin_fp_screen_releases(false, true, false, false), "native full screen: nothing to release");
    CHECK(!pingouin_fp_screen_resumes(true, false, true), "windowed again: no resume");
}

/*
 * The guest's size in full screen (ui/pingouin-clean-size.h picks it; the
 * rule here says whether it may cut rows). Over the whole panel it is the
 * whole panel in pixels: on the MacBook Air the cut made 2940x1900 of a
 * 2940x1912 panel, 12 black rows across the bottom. Below the notch
 * (native) the cut stays, as before.
 */
static void test_guest_size(void)
{
    for (size_t i = 0; i < 4; i++) {
        const Display *d = &builtins[i];
        double backing = 2;
        double pw = d->frame.w * backing, ph = d->frame.h * backing;
        bool usable = fallback(d) == PINGOUIN_FP_OK;
        bool cut = pingouin_fp_cut_rows(true, true, d->notch, usable);
        PingouinSize s = pingouin_clean_size(pw, ph, backing, cut);

        CHECK(!cut, "%s FullPanel: no rows cut", d->name);
        CHECK(s.w == (uint32_t)pw && s.h == (uint32_t)ph,
              "%s FullPanel: guest %ux%u, want the whole panel %.0fx%.0f",
              d->name, s.w, s.h, pw, ph);

        /* Native: the area below the notch, a few rows cut for the presets. */
        double nh = (d->frame.h - d->notch) * backing;
        cut = pingouin_fp_cut_rows(true, true, d->notch, false);
        s = pingouin_clean_size(pw, nh, backing, cut);
        CHECK(cut, "%s native: rows may be cut", d->name);
        CHECK(s.w == (uint32_t)pw && s.h <= (uint32_t)nh &&
              s.h + PINGOUIN_TRIM_MAX_PT * backing >= nh,
              "%s native: guest %ux%u in %.0fx%.0f", d->name, s.w, s.h, pw, nh);
    }

    /* The bug: the cut over the whole Air panel gave 2940x1900. */
    PingouinSize old = pingouin_clean_size(2940, 1912, 2, true);
    CHECK(old.h == 1900, "Air panel with the cut: %u rows (the black band)", old.h);
    PingouinSize air = pingouin_clean_size(2940, 1912, 2,
                                       pingouin_fp_cut_rows(true, true, 32, true));
    CHECK(air.w == 2940 && air.h == 1912, "Air FullPanel: %ux%u", air.w, air.h);

    /* No cut without the layer (GPU safe mode), in a window, or without a notch. */
    CHECK(!pingouin_fp_cut_rows(true, false, 32, false), "no layer: nothing cut");
    CHECK(!pingouin_fp_cut_rows(false, true, 32, false), "window: nothing cut");
    CHECK(!pingouin_fp_cut_rows(true, true, 0, false), "no notch: nothing cut");
}

static bool move_to(bool asked, const Display *d, double below_top, bool shown)
{
    double top = d->frame.y + d->frame.h;
    return pingouin_fp_asked_after_move(asked, pingouin_fp_zone(top - below_top, top, d->notch), shown);
}

static void test_reveal(void)
{
    const Display *d = &builtins[0];
    double top = d->frame.y + d->frame.h;
    bool asked = false;

    CHECK(pingouin_fp_zone(top - 0.2, top, d->notch) == PINGOUIN_FP_ZONE_TOP_ROW, "top row");
    CHECK(pingouin_fp_zone(top - 10, top, d->notch) == PINGOUIN_FP_ZONE_STRIP, "strip");
    CHECK(pingouin_fp_zone(top - d->notch - 1, top, d->notch) == PINGOUIN_FP_ZONE_PICTURE, "picture");

    asked = move_to(asked, d, 200, false);
    CHECK(!asked, "pointer in the VM: menu bar held back");
    asked = move_to(asked, d, 10, false);
    CHECK(!asked, "pointer in the strip: still held back (the strip is the VM's)");
    asked = move_to(asked, d, 0.2, false);
    CHECK(asked, "pointer at the top row: asked for");
    asked = move_to(asked, d, 10, false);
    CHECK(asked, "back in the strip: still asked");
    asked = move_to(asked, d, 300, true);
    CHECK(asked, "menu open: stays while the pointer is in a menu");
    asked = move_to(asked, d, d->notch + 1, false);
    CHECK(!asked, "below the strip, menu bar gone: held back again");

    /* AppKit's own reveal on the way into full screen (pointer elsewhere): not shown. */
    CHECK(!pingouin_fp_reveal_shown(true, false), "AppKit's reveal, not allowed (entry, Keep the Dock away): stays hidden");
    CHECK(pingouin_fp_reveal_shown(true, true), "AppKit's reveal after the pointer touched the top row: shown");
    CHECK(!pingouin_fp_reveal_shown(false, true) && !pingouin_fp_reveal_shown(false, false), "no reveal: nothing shown");
}

static PingouinFPFacts covering(bool dock_away)
{
    PingouinFPFacts f = { .covering = true, .key = true, .on_space = true, .reveal = !dock_away };
    return f;
}

/*
 * What the menu bar's backdrop, the app's options and the titlebar window
 * should be, with "Keep the Dock away" on (QEMU's immersive=on, the
 * default: the menu bar never comes down, as in normal full screen) and off
 * (it comes down at the top edge: macOS's own auto-hide once the pointer
 * touched the top row).
 */
static void test_intent(void)
{
    for (int i = 0; i < 2; i++) {
        bool dock_away = i == 0;
        const char *name = dock_away ? "Dock away on" : "Dock away off";
        PingouinFPFacts f = covering(dock_away);
        PingouinFPIntent in = pingouin_fp_intent(f);

        /* The pointer in the VM: held back, backdrop off, clicks to the VM. */
        CHECK(in.options == PINGOUIN_FP_OPTIONS_HOLD, "%s, at rest: held back", name);
        CHECK(!in.menu_bar_opaque && in.pass_through, "%s, at rest: the strip is the VM's", name);
        f.holding = true;

        /* AppKit's own reveal on the way in, nothing asked: still the VM's strip. */
        f.shown = true;
        in = pingouin_fp_intent(f);
        CHECK(!in.menu_bar_opaque && in.options == PINGOUIN_FP_OPTIONS_HOLD,
              "%s, AppKit's own reveal: not shown, held back", name);
        f.shown = false;

        /* The top row touched. */
        f.asked = true;
        in = pingouin_fp_intent(f);
        if (dock_away) {
            /* The menu bar never comes down (3.0.16 brought it down). */
            CHECK(in.options == PINGOUIN_FP_OPTIONS_HOLD, "%s, top row: still held back", name);
        } else {
            CHECK(in.options == PINGOUIN_FP_OPTIONS_RELAX, "%s, top row: relaxed, macOS brings it down", name);
            f.holding = false;
        }
        CHECK(!in.menu_bar_opaque, "%s, top row: backdrop not yet (no reveal on screen)", name);

        /* The reveal on screen. */
        f.shown = true;
        in = pingouin_fp_intent(f);
        CHECK(in.menu_bar_opaque == !dock_away && in.pass_through == dock_away,
              "%s, reveal on screen: menu bar %s", name, dock_away ? "still the VM's" : "macOS's");

        /* It went, the pointer back in the VM: held back again. */
        f.shown = false;
        f.asked = false;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_HOLD && !in.menu_bar_opaque, "%s, hidden again", name);
        f.holding = true;

        /* Another app or window is key: relaxed, once. */
        f.key = false;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_RELAX, "%s, not key: relaxed", name);
        f.holding = false;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_LEAVE, "%s, not key: then left alone", name);
        f.key = true;

        /* Mission Control or another Space: the system's transition, left alone. */
        f.on_space = false;
        f.holding = true;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_LEAVE, "%s, away: options left alone", name);
        CHECK(!in.menu_bar_opaque, "%s, away: backdrop stays off (set again while shown in MC)", name);
        f.on_space = true;

        /* A return's one-turn release. */
        f.releasing = true;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_RELAX, "%s, return release: relaxed", name);
        f.releasing = false;

        /* Not covering any more (left, lost, released): relaxed, macOS's backdrop. */
        f.covering = false;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_RELAX && in.menu_bar_opaque && !in.pass_through,
              "%s, not covering: back to macOS's", name);
        f.holding = false;
        in = pingouin_fp_intent(f);
        CHECK(in.options == PINGOUIN_FP_OPTIONS_LEAVE, "%s, not covering, nothing held: left alone", name);
    }
}

#define HELD PINGOUIN_FP_HELD_BACK

static void test_options(void)
{
    const uint64_t immersive = PINGOUIN_FP_FULL_SCREEN | PINGOUIN_FP_HIDE_DOCK |
                               PINGOUIN_FP_HIDE_MENU_BAR;
    const uint64_t plain = PINGOUIN_FP_FULL_SCREEN | PINGOUIN_FP_AUTO_HIDE_DOCK |
                           PINGOUIN_FP_AUTO_HIDE_MENU_BAR;
    uint64_t out = 0;

    CHECK(HELD == (PINGOUIN_FP_FULL_SCREEN | PINGOUIN_FP_HIDE_MENU_BAR | PINGOUIN_FP_HIDE_DOCK),
          "held back: full screen, menu bar and Dock hidden");
    /* Keep the Dock away: QEMU's own full-screen options are the held-back ones. */
    CHECK(immersive == HELD, "Dock away's own options are the held-back ones");

    /* The full screen's own options are read only in full screen, never the held-back ones. */
    CHECK(pingouin_fp_own_options(true, plain), "AppKit's own full-screen options");
    CHECK(!pingouin_fp_own_options(true, HELD), "held back: not the own ones");
    CHECK(!pingouin_fp_own_options(false, 0), "windowed: not the own ones");
    CHECK(!pingouin_fp_own_options(true, 0), "full screen bit missing: not yet");

    /* Relaxing, Dock away off: the own options, menu bar auto-hidden, Dock as it was. */
    out = pingouin_fp_relax_target(true, plain, 0, true);
    CHECK(out == plain, "Dock away off: own 0x%llx", (unsigned long long)out);
    out = pingouin_fp_relax_target(false, 0, PINGOUIN_FP_FULL_SCREEN | PINGOUIN_FP_HIDE_DOCK, true);
    CHECK((out & PINGOUIN_FP_AUTO_HIDE_MENU_BAR) && !(out & PINGOUIN_FP_HIDE_MENU_BAR) &&
          (out & PINGOUIN_FP_HIDE_DOCK) && (out & PINGOUIN_FP_FULL_SCREEN),
          "own not known: from before, made revealable, the Dock kept (0x%llx)",
          (unsigned long long)out);
    out = pingouin_fp_relaxed(PINGOUIN_FP_FULL_SCREEN);
    CHECK(out & PINGOUIN_FP_AUTO_HIDE_DOCK,
          "no Dock setting: auto-hidden Dock added (0x%llx)", (unsigned long long)out);
    /* Dock away on: never revealable; relaxing gives QEMU's own (the held-back ones). */
    out = pingouin_fp_relax_target(false, 0, immersive, false);
    CHECK(out == immersive, "Dock away on: own 0x%llx", (unsigned long long)out);
    CHECK(!(out & PINGOUIN_FP_AUTO_HIDE_MENU_BAR), "Dock away on: never auto-hidden");

    /* Letting go. */
    CHECK(pingouin_fp_restore(true, true, plain, 0, &out) && out == plain, "in full screen: own");
    CHECK(!pingouin_fp_restore(true, false, 0, 0, &out), "own not known: AppKit's (nothing set)");
    CHECK(pingouin_fp_restore(false, false, 0, 7, &out) && out == 7, "windowed: from before");
}

/* Mission Control and Space returns: wait for WindowServer, never force. */
static void test_returns(void)
{
    const Display *d = &builtins[2];
    double primary_top = d->frame.y + d->frame.h;
    PingouinFPRect ws = { 0, 0, d->frame.w, d->frame.h };
    PingouinFPRect moving = { 200, 150, d->frame.w * 0.6, d->frame.h * 0.6 };

    CHECK(pingouin_fp_compositor_matches(ws, d->frame, primary_top), "WindowServer has it in place");
    CHECK(!pingouin_fp_compositor_matches(moving, d->frame, primary_top), "Mission Control still moves it");
    /* A built-in display beside the main one: WindowServer's top-left origin. */
    PingouinFPRect beside = builtins[4].frame;
    PingouinFPRect ws_beside = { beside.x, 1440 - (beside.y + beside.h), beside.w, beside.h };
    CHECK(pingouin_fp_compositor_matches(ws_beside, beside, 1440), "beside an external display");

    CHECK(pingouin_fp_return_ready(true, true, true, 1, PINGOUIN_FP_RETURN_CHECKS), "in place: ready");
    CHECK(!pingouin_fp_return_ready(true, true, true, 0, 10), "still moving: wait");
    CHECK(!pingouin_fp_return_ready(true, false, true, 1, 10), "not its Space: wait");
    CHECK(!pingouin_fp_return_ready(true, true, false, 1, 10), "not key: wait");
    CHECK(!pingouin_fp_return_ready(false, true, true, 1, 10), "not covering: never");
    CHECK(!pingouin_fp_return_ready(true, true, true, -1, PINGOUIN_FP_RETURN_CHECKS),
          "not known: not at once");
    CHECK(pingouin_fp_return_ready(true, true, true, -1, PINGOUIN_FP_RETURN_UNKNOWN_AFTER),
          "not known: a settled key window after a while");

    CHECK(pingouin_fp_return_again(true, true, true, true, true, false),
          "started while away, the real return: once more");
    CHECK(!pingouin_fp_return_again(true, true, true, true, true, true), "only once");
    CHECK(!pingouin_fp_return_again(true, false, true, true, true, false), "not from away: no");
    CHECK(!pingouin_fp_return_again(true, true, true, false, true, false), "still away: no");

    CHECK(pingouin_fp_return_reassert(true, true, true, false, false), "from away, held back: reassert");
    CHECK(!pingouin_fp_return_reassert(true, false, true, false, false),
          "no reveal (Keep the Dock away): nothing to reassert");
    CHECK(!pingouin_fp_return_reassert(true, true, true, true, false), "a reveal on screen: left alone");
    CHECK(!pingouin_fp_return_reassert(true, true, true, false, true), "once");
    CHECK(!pingouin_fp_return_reassert(false, true, true, false, false), "not from away: no");
}

int main(void)
{
    test_requested();
    test_fallback();
    test_menu_bar_setting();
    test_hook();
    test_verdict();
    test_lost();
    test_moves();
    test_guest_size();
    test_reveal();
    test_intent();
    test_options();
    test_returns();
    if (failures) {
        printf("test-fullpanel: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test-fullpanel: %d checks passed\n", checks);
    return 0;
}
