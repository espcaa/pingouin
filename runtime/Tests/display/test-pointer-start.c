/*
 * Unit test for ui/pingouin-pointer-start.h (the VM takes the pointer without
 * a click; the Mac's cursor hides only once the guest draws its own).
 *
 * A small model of QEMU's Cocoa view stands in for ui/cocoa.m: the same
 * moments call the header the way ui/cocoa.m does (a motion over the view,
 * the window key, the app active, full screen, the mode change, the guest's
 * hello and reset, Ctrl+Alt+G), and AppKit's side is modelled as far as it
 * matters here: a motion reaches the view only while its window is key,
 * "entered" only when the pointer crosses into the view, a click grabs.
 * The guest gets a motion only while the VM has the pointer and its tablet
 * is live. [NSCursor hide] is a counter, as on macOS.
 *
 * The window server's side of the cursor is modelled too: what it shows
 * (AppKit's current cursor, or an arrow macOS put there itself) and
 * whether another process or macOS showed the cursor while QEMU still
 * counts it hidden (the duplicate Mac cursor of 2026-10-19). old_image
 * runs the cursor as 3.1.24 did (hidden, its image left alone), so the
 * test also shows that report in the model.
 *
 * Each case runs the new code and upstream's (off: PINGOUIN_POINTER_START=0,
 * which is upstream's way) and checks both, so the test also shows the
 * user's 15:00 report in the model: after a start or a guest reboot the
 * guest's pointer did not move until a click.
 *
 * cc -Wall -Werror -I<qemu>/ui test-pointer-start.c -o t && ./t
 */
#include <stdio.h>
#include <string.h>

#include "pingouin-pointer-start.h"

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

/* What an image looks like on screen. */
typedef enum Img { IMG_ARROW, IMG_EMPTY, IMG_GUEST } Img;

typedef struct Vm {
    PingouinPointerStart ptr;
    bool hide_setting;  /* show-cursor=off (guest-pointer) */
    bool absolute;      /* the guest's pointer is absolute (virtio-tablet) */
    bool grabbed;
    bool active, key;
    bool over;          /* the pointer is on the view */
    bool buttons;
    bool full;
    bool tablet_live;   /* the guest's driver is up (events not dropped) */
    bool cursor_hidden; /* ours (pingouin_mac_cursor_hidden) */
    int hide_count;     /* macOS's: [NSCursor hide] minus unhide */
    int guest_moves;    /* motions the guest got */
    bool old_image;     /* 3.1.24: the image left alone while hidden */
    PingouinCursorImage want; /* the image QEMU wants (pingouin_cur_want) */
    bool qemu_set;      /* QEMU set an image of its own (pingouin_cur_set) */
    Img appkit;         /* AppKit's current cursor ([NSCursor currentCursor]) */
    Img shown_image;    /* what the window server shows when the cursor shows */
    bool revealed;      /* another process or macOS showed the hidden cursor */
    double now, last_look;
} Vm;

static Img img_of(PingouinCursorImage w)
{
    return w == PINGOUIN_CURSOR_EMPTY ? IMG_EMPTY : w == PINGOUIN_CURSOR_GUEST ? IMG_GUEST : IMG_ARROW;
}

/*
 * pingouin_cursor_image: its images and the arrow only over our window;
 * force: the other empty one. A set after QEMU's arrow was left out
 * reaches the screen (the arrow first).
 */
static void set_image(Vm *v, PingouinCursorImage want, bool force)
{
    v->want = want;
    if (want == PINGOUIN_CURSOR_APPKIT) {
        if (v->qemu_set) {
            v->qemu_set = false;
            if (v->over) {
                v->appkit = v->shown_image = IMG_ARROW;
            }
        }
    } else if ((v->appkit != img_of(want) || !v->qemu_set || force) && v->over) {
        v->appkit = v->shown_image = img_of(want);
        v->qemu_set = true;
    }
}

/* The window server shows the cursor: QEMU did not hide it, or someone showed it. */
static bool ws_visible(const Vm *v)
{
    return v->hide_count <= 0 || v->revealed;
}

/* The user sees the Mac's arrow (or another image) where the guest draws its pointer. */
static bool mac_arrow_seen(const Vm *v)
{
    return ws_visible(v) && v->shown_image != IMG_EMPTY;
}

static void update_cursor(Vm *v)
{
    bool hide;

    if (v->ptr.off) {
        /* upstream: hideCursor in every grab, unhideCursor in every ungrab */
        return;
    }
    hide = pingouin_pointer_hide_mac(&v->ptr, v->hide_setting, v->grabbed, v->absolute);
    if (!v->old_image) {
        set_image(v, pingouin_pointer_image(false, false, hide), false);
    }
    if (hide != v->cursor_hidden) {
        v->cursor_hidden = hide;
        v->hide_count += hide ? 1 : -1;
        if (hide) {
            v->revealed = false;    /* QEMU's own hide hides it again */
        }
    }
}

/* pingouin_cursor_again: a motion while the VM has the pointer, or the watch. */
static void cursor_again(Vm *v)
{
    bool has, looked = false, shown = false;

    if (v->old_image || v->ptr.off || v->want == PINGOUIN_CURSOR_APPKIT) {
        return;
    }
    has = v->appkit == img_of(v->want);
    if (has && v->cursor_hidden && pingouin_pointer_look_due(v->now, &v->last_look)) {
        looked = true;
        shown = ws_visible(v);
    }
    if (pingouin_pointer_image_again(v->want, has, v->cursor_hidden, looked, shown)) {
        set_image(v, v->want, has);
    }
}

static void grab(Vm *v)
{
    if (v->ptr.off && v->hide_setting) {
        v->hide_count++;
    }
    v->grabbed = true;
    pingouin_pointer_left(&v->ptr);
    update_cursor(v);
}

static void ungrab(Vm *v)
{
    if (v->ptr.off && v->hide_setting) {
        v->hide_count--;
    }
    v->grabbed = false;
    update_cursor(v);
}

static void take(Vm *v)
{
    PingouinPointerIn in = {
        .absolute = v->absolute, .grabbed = v->grabbed, .active = v->active,
        .key = v->key, .over = v->over, .buttons = v->buttons,
    };

    if (pingouin_pointer_take(&v->ptr, &in)) {
        grab(v);
    }
}

static Vm vm(bool off)
{
    Vm v;

    memset(&v, 0, sizeof(v));
    v.ptr.off = off;
    v.hide_setting = true;
    v.absolute = true;  /* virtio-tablet is registered from the start */
    v.now = 100;        /* seconds since the Mac started */
    return v;
}

static bool mac_cursor_shown(const Vm *v)
{
    return v->hide_count <= 0;
}

/* The mouse moved with the pointer on the view (AppKit: only to the key window). */
static void motion(Vm *v)
{
    if (!v->key) {
        return;
    }
    if (!v->grabbed && !v->ptr.off) {
        take(v);
    }
    if (v->grabbed) {
        cursor_again(v);
    }
    if (v->grabbed && v->tablet_live) {
        v->guest_moves++;
    }
}

/* Another process shows the cursor (Pingouin Gestures' background show, B3 on the Air). */
static void background_show(Vm *v)
{
    v->revealed = true;
}

/* macOS shows its own arrow (around a Space switch or the menu bar); AppKit sees nothing. */
static void macos_arrow(Vm *v)
{
    v->revealed = true;
    v->shown_image = IMG_ARROW;
}

/* AppKit puts its arrow back (the pointer at a window's edge). */
static void appkit_arrow(Vm *v)
{
    v->appkit = v->shown_image = IMG_ARROW;
}

/* The pointer crossed into the view (mouseEntered:, key window only). */
static void enter(Vm *v)
{
    v->over = true;
    if (v->key && v->absolute && !v->grabbed) {
        grab(v);
    }
    motion(v);
}

/* ... and out of it (mouseExited:). */
static void leave(Vm *v)
{
    v->over = false;
    v->shown_image = IMG_ARROW;     /* macOS's own outside our windows (seen on macOS 27) */
    pingouin_pointer_left(&v->ptr);
    if (v->key && v->absolute && v->grabbed) {
        ungrab(v);
    }
}

/*
 * Up into Omanotch's strip (full screen below a notch): the strip sets its
 * own cursor (the guest's arrow) as the pointer enters it; QEMU lets go.
 */
static void strip_up(Vm *v)
{
    v->over = false;
    v->shown_image = IMG_GUEST;
    pingouin_pointer_left(&v->ptr);
    if (v->key && v->absolute && v->grabbed) {
        ungrab(v);
    }
}

/* Down out of the strip: Omanotch sets the Mac's arrow for Pingouin, the view is entered. */
static void strip_down(Vm *v)
{
    v->shown_image = IMG_ARROW;
    enter(v);
}

/* The watch while hidden (pingouin_cursor_watch): one tick, no motion. */
static void watch_tick(Vm *v)
{
    v->now += PINGOUIN_POINTER_WATCH_S;
    if (v->cursor_hidden) {
        cursor_again(v);
    }
}

/* AppKit's exited + entered without the pointer moving (a resize). */
static void view_changed(Vm *v)
{
    if (v->key && v->absolute && v->grabbed) {
        ungrab(v);
    }
}

static void click(Vm *v)
{
    if (!v->key) {
        v->active = v->key = true;  /* the click brings the app in front */
        if (!v->ptr.off) {
            v->buttons = true;      /* key and active come during the press */
            take(v);
            v->buttons = false;
        }
    }
    if (!v->grabbed) {
        grab(v);                    /* mouseUp: */
    }
}

static void become_key(Vm *v)
{
    v->active = v->key = true;
    if (!v->ptr.off) {
        take(v);
    }
}

static void resign_key(Vm *v)
{
    v->active = v->key = false;
    pingouin_pointer_left(&v->ptr);
    ungrab(v);
}

static void full_screen(Vm *v)
{
    v->full = true;
    if (v->grabbed && !v->ptr.off) {
        return;
    }
    if (v->ptr.off || !v->absolute) {
        grab(v);    /* upstream: grabs, even when it has the pointer already */
        return;
    }
    take(v);
}

static void let_go(Vm *v)    /* Ctrl+Alt+G */
{
    pingouin_pointer_let_go(&v->ptr);
    ungrab(v);
}

static void guest_reset(Vm *v)
{
    v->tablet_live = false;
    pingouin_pointer_guest_reset(&v->ptr);
    update_cursor(v);
}

static void tablet_up(Vm *v)
{
    v->tablet_live = true;
}

static void hello(Vm *v)
{
    pingouin_pointer_guest_hello(&v->ptr);
    update_cursor(v);
    if (!v->ptr.off) {
        take(v);
    }
}

/* A window opening under the pointer: no "entered" (the pointer did not move in). */
static void open_under_pointer(Vm *v)
{
    v->over = true;
}

static void boot(Vm *v)
{
    tablet_up(v);
    hello(v);
}

static void test_windowed_start(void)
{
    for (int off = 0; off <= 1; off++) {
        Vm v = vm(off);

        open_under_pointer(&v);
        become_key(&v);         /* Pingouin.app hands over the focus */
        CHECK(mac_cursor_shown(&v), "windowed start, booting (off=%d): Mac cursor shown", off);
        boot(&v);
        motion(&v);
        motion(&v);
        if (!off) {
            CHECK(v.guest_moves == 2, "windowed start: the first motions reach the guest (%d)", v.guest_moves);
            CHECK(!mac_cursor_shown(&v), "windowed start: the guest draws, the Mac cursor hidden");
        } else {
            CHECK(v.guest_moves == 0, "upstream, windowed start: no motion before a click (%d)", v.guest_moves);
            click(&v);
            motion(&v);
            CHECK(v.guest_moves == 1, "upstream: after the click it moves (%d)", v.guest_moves);
        }
    }
}

static void test_windowed_start_motion_during_boot(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    become_key(&v);
    motion(&v);             /* firmware: the tablet is not live yet */
    CHECK(v.grabbed, "booting: the VM has the pointer");
    CHECK(mac_cursor_shown(&v), "booting: the guest draws none, the Mac cursor shown");
    CHECK(v.guest_moves == 0, "booting: nothing reaches the guest yet");
    tablet_up(&v);
    motion(&v);
    CHECK(v.guest_moves == 1, "tablet up: the next motion reaches the guest");
    CHECK(mac_cursor_shown(&v), "no hello yet: the Mac cursor still shown");
    hello(&v);
    CHECK(!mac_cursor_shown(&v), "hello: the Mac cursor hidden");
}

static void test_full_screen_start(void)
{
    for (int off = 0; off <= 1; off++) {
        Vm v = vm(off);

        open_under_pointer(&v);
        full_screen(&v);        /* full-screen=on: before the app is in front */
        if (off) {
            CHECK(v.grabbed && !mac_cursor_shown(&v),
                  "upstream: full screen grabs and hides the Mac cursor while booting");
        } else {
            CHECK(!v.grabbed && mac_cursor_shown(&v),
                  "full screen before the focus: not taken yet, the Mac cursor shown");
        }
        /*
         * The launcher's hand-over: QEMU's window is key for a moment, the
         * launcher takes it back, then yields it to QEMU.
         */
        become_key(&v);
        resign_key(&v);
        become_key(&v);
        boot(&v);
        motion(&v);
        if (!off) {
            CHECK(v.grabbed && v.guest_moves == 1, "full-screen start: the first motion reaches the guest");
            CHECK(!mac_cursor_shown(&v), "full-screen start: Mac cursor hidden once the guest draws");
        } else {
            CHECK(v.guest_moves == 0, "upstream full-screen start: static until a click (%d)", v.guest_moves);
        }
    }
}

static void test_full_screen_pointer_elsewhere(void)
{
    Vm v = vm(false);

    become_key(&v);         /* the pointer is on another display */
    full_screen(&v);
    boot(&v);
    CHECK(!v.grabbed, "pointer on another display: not taken");
    CHECK(mac_cursor_shown(&v), "pointer on another display: the Mac cursor shown there");
    enter(&v);
    CHECK(v.grabbed && v.guest_moves == 1, "moved onto the VM: taken, motion in the guest");
}

static void test_reboot(void)
{
    for (int off = 0; off <= 1; off++) {
        Vm v = vm(off);

        open_under_pointer(&v);
        become_key(&v);
        boot(&v);
        click(&v);
        full_screen(&v);
        motion(&v);
        int before = v.guest_moves;

        guest_reset(&v);
        if (!off) {
            CHECK(mac_cursor_shown(&v), "reboot: the guest draws no pointer, the Mac cursor shown");
            CHECK(v.hide_count == 0, "reboot: hide and unhide balanced (%d)", v.hide_count);
        }
        view_changed(&v);       /* the guest's screen goes away and comes back */
        tablet_up(&v);
        motion(&v);
        motion(&v);
        if (!off) {
            CHECK(v.guest_moves == before + 2, "after a reboot the motion reaches the guest without a click");
            CHECK(mac_cursor_shown(&v), "after a reboot, before hello: the Mac cursor shown");
            hello(&v);
            CHECK(!mac_cursor_shown(&v) && v.hide_count == 1,
                  "hello after the reboot: hidden once (%d)", v.hide_count);
        } else {
            CHECK(v.guest_moves == before, "upstream after the reboot: static until a click");
            CHECK(v.hide_count == 1, "upstream: full screen grabbed twice, one ungrab: still hidden (%d)",
                  v.hide_count);
        }
    }
}

static void test_key_again(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    resign_key(&v);         /* Command-Tab away, or the escape combo */
    CHECK(!v.grabbed && mac_cursor_shown(&v), "away: the pointer and the Mac cursor are macOS's");
    motion(&v);
    CHECK(v.guest_moves == 1, "away: no motion to the guest");
    become_key(&v);         /* back, the pointer still on the VM */
    CHECK(v.grabbed && !mac_cursor_shown(&v), "back: taken at once");
    motion(&v);
    CHECK(v.guest_moves == 2, "back: motion reaches the guest");
}

static void test_not_in_front(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    boot(&v);
    v.key = true;           /* a key window, but another app in front */
    v.active = false;
    take(&v);
    CHECK(!v.grabbed, "another app in front: not taken");
    v.active = true;
    v.key = false;
    take(&v);
    CHECK(!v.grabbed, "no window of ours key: not taken");
}

static void test_let_go(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    let_go(&v);
    motion(&v);
    motion(&v);
    CHECK(!v.grabbed && v.guest_moves == 1, "Ctrl+Alt+G: stays let go while on the VM");
    CHECK(mac_cursor_shown(&v), "let go: the Mac cursor shown");
    become_key(&v);
    CHECK(!v.grabbed, "let go: the window key again does not take it either");
    leave(&v);
    enter(&v);
    CHECK(v.grabbed, "left and came back: taken (entered)");
    let_go(&v);
    leave(&v);
    v.over = true;          /* back on the VM without an entered event */
    motion(&v);
    CHECK(v.grabbed, "left and came back: the first motion takes it");
    let_go(&v);
    click(&v);
    CHECK(v.grabbed, "let go, then a click: taken");
    let_go(&v);
    resign_key(&v);
    become_key(&v);
    CHECK(v.grabbed, "let go, away and back: taken");
}

static void test_buttons(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    v.active = true;
    v.key = true;
    v.buttons = true;       /* a drag that started elsewhere */
    boot(&v);
    motion(&v);
    CHECK(!v.grabbed, "a button held: not taken by motion");
    v.buttons = false;
    click(&v);
    CHECK(v.grabbed, "the click's mouseUp takes it (as before)");
}

static void test_click_to_front(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    boot(&v);
    click(&v);              /* the activating click: taken by its mouseUp */
    CHECK(v.grabbed && v.hide_count == 1, "click to the front: taken once (%d)", v.hide_count);
}

static void test_relative(void)
{
    Vm v = vm(false);

    v.absolute = false;     /* a relative mouse only (not Pingouin's VMs) */
    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    CHECK(!v.grabbed, "relative pointer: never taken by motion (it would capture the cursor)");
    full_screen(&v);
    CHECK(v.grabbed, "relative pointer: full screen grabs it, as upstream");
    CHECK(!mac_cursor_shown(&v), "relative pointer: the held cursor is hidden");
    guest_reset(&v);
    CHECK(!mac_cursor_shown(&v), "relative pointer: hidden through a reset (it stands still)");
}

static void test_show_cursor_on(void)
{
    Vm v = vm(false);

    v.hide_setting = false; /* show-cursor=on: VMs without guest-pointer */
    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    CHECK(v.grabbed && v.guest_moves == 1, "show-cursor=on: taken without a click too");
    CHECK(mac_cursor_shown(&v) && v.hide_count == 0, "show-cursor=on: the Mac cursor never hidden");
}

static void test_hello_repeats(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    hello(&v);              /* the display agent restarted */
    hello(&v);
    CHECK(v.hide_count == 1, "hello again: still hidden once (%d)", v.hide_count);
    ungrab(&v);
    CHECK(v.hide_count == 0, "let go: shown");
    ungrab(&v);
    CHECK(v.hide_count == 0, "a second let-go: no extra unhide (%d)", v.hide_count);
}

static void test_hello_takes(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    v.active = v.key = true;    /* key, but the user has not moved since */
    tablet_up(&v);
    hello(&v);
    CHECK(v.grabbed && !mac_cursor_shown(&v), "hello with the pointer on the key VM: taken, hidden");
}

static void test_image_rules(void)
{
    double last = -1;

    CHECK(pingouin_pointer_image(false, false, false) == PINGOUIN_CURSOR_APPKIT,
          "not hidden: AppKit's cursor (the arrow)");
    CHECK(pingouin_pointer_image(false, true, true) == PINGOUIN_CURSOR_EMPTY,
          "hidden: an empty image too");
    CHECK(pingouin_pointer_image(true, true, false) == PINGOUIN_CURSOR_GUEST,
          "hardware cursor: the guest's image");
    CHECK(pingouin_pointer_image(true, false, false) == PINGOUIN_CURSOR_EMPTY,
          "hardware cursor, the guest hides its pointer: empty");
    CHECK(pingouin_pointer_image(true, true, true) == PINGOUIN_CURSOR_GUEST,
          "hardware cursor: the guest's image (never hidden)");

    CHECK(pingouin_pointer_look_due(10.0, &last) && last == 10.0, "the first look is due");
    CHECK(!pingouin_pointer_look_due(10.05, &last) && last == 10.0, "not again within 0.1 s");
    CHECK(pingouin_pointer_look_due(10.11, &last) && last == 10.11, "again after 0.1 s");
    CHECK(pingouin_pointer_look_due(5.0, &last) && last == 5.0, "the clock went back: due");

    CHECK(!pingouin_pointer_image_again(PINGOUIN_CURSOR_APPKIT, false, false, true, true),
          "AppKit's own: never set by QEMU");
    CHECK(pingouin_pointer_image_again(PINGOUIN_CURSOR_EMPTY, false, true, false, false),
          "AppKit put its arrow back: set again at once");
    CHECK(pingouin_pointer_image_again(PINGOUIN_CURSOR_GUEST, false, false, false, false),
          "hardware cursor, AppKit put its arrow back: set again at once");
    CHECK(!pingouin_pointer_image_again(PINGOUIN_CURSOR_EMPTY, true, true, true, false),
          "hidden, the window server says hidden: nothing to do");
    CHECK(pingouin_pointer_image_again(PINGOUIN_CURSOR_EMPTY, true, true, true, true),
          "hidden, but the window server shows it: set again");
    CHECK(!pingouin_pointer_image_again(PINGOUIN_CURSOR_EMPTY, true, true, false, true),
          "... only when it was asked (at most every 0.1 s)");
    CHECK(!pingouin_pointer_image_again(PINGOUIN_CURSOR_EMPTY, true, false, true, true),
          "the guest hides its hardware cursor (QEMU did not hide): no look");
}

/* Started windowed, booted, the pointer on the VM: taken, the guest draws, the Mac cursor hidden. */
static Vm running_vm(bool old_image)
{
    Vm v = vm(false);

    v.old_image = old_image;
    open_under_pointer(&v);
    become_key(&v);
    boot(&v);
    motion(&v);
    return v;
}

static void test_background_show(void)
{
    for (int old = 0; old <= 1; old++) {
        Vm v = running_vm(old);

        CHECK(v.grabbed && v.cursor_hidden && !mac_arrow_seen(&v), "running (old=%d): no Mac arrow", old);
        background_show(&v);    /* Pingouin Gestures shows the cursor from the background */
        for (int i = 0; i < 5; i++) {
            v.now += 0.25;
            motion(&v);
        }
        if (old) {
            CHECK(mac_arrow_seen(&v), "3.1.24: a background show brings the arrow over the VM, moves keep it");
            resign_key(&v);     /* the user's way out: away and back */
            become_key(&v);
            CHECK(!mac_arrow_seen(&v), "3.1.24: away and back hides it again");
        } else {
            CHECK(!mac_arrow_seen(&v), "a background show: the cursor shows its empty image, no arrow");
        }
        CHECK(v.grabbed && v.guest_moves == 6, "the guest's pointer keeps moving (%d)", v.guest_moves);
        let_go(&v);
        CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW, "Ctrl+Alt+G after it (old=%d): the arrow", old);
        CHECK(v.hide_count <= 0, "hide and unhide stay balanced (%d)", v.hide_count);
    }
}

static void test_macos_arrow(void)
{
    Vm v = running_vm(false);

    v.now += 1;
    motion(&v);             /* a look: hidden, all fine */
    macos_arrow(&v);        /* macOS shows its arrow; AppKit still has the empty one */
    CHECK(mac_arrow_seen(&v), "macOS showed its arrow (at rest it stays until the next motion)");
    v.now += 0.02;
    motion(&v);
    CHECK(mac_arrow_seen(&v), "a motion within 0.1 s of the last look: not asked yet");
    v.now += 0.1;
    motion(&v);
    CHECK(!mac_arrow_seen(&v) && v.appkit == IMG_EMPTY,
          "the next look sees it shown: the empty image again, no arrow");
    CHECK(v.cursor_hidden && v.hide_count == 1, "no second hide (%d)", v.hide_count);
}

static void test_appkit_arrow(void)
{
    Vm v = running_vm(false);

    appkit_arrow(&v);       /* at a window's edge */
    CHECK(!mac_arrow_seen(&v), "AppKit's arrow while hidden: not seen");
    motion(&v);
    CHECK(v.appkit == IMG_EMPTY, "the next motion sets the empty image again at once");
    background_show(&v);
    CHECK(!mac_arrow_seen(&v), "so a show after it shows nothing");
}

/* B5: a show while the pointer rests (the user types); no motion follows. */
static void test_show_at_rest(void)
{
    Vm v = running_vm(false);

    v.now += 1;
    macos_arrow(&v);        /* or an empty image set while another process hid it: the arrow shows */
    CHECK(mac_arrow_seen(&v), "a show at rest: the arrow over the VM");
    watch_tick(&v);
    CHECK(!mac_arrow_seen(&v) && v.appkit == IMG_EMPTY, "the watch sets the empty image within 0.5 s, no motion");
    CHECK(v.cursor_hidden && v.hide_count == 1, "no second hide (%d)", v.hide_count);
    let_go(&v);
    for (int i = 0; i < 4; i++) {
        watch_tick(&v);
    }
    CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW, "let go: no watch, the arrow stays");
}

/* Up into Omanotch's strip and back: the strip's own cursor stays, the VM's empty one comes back. */
static void test_strip(void)
{
    Vm v = running_vm(false);

    full_screen(&v);
    strip_up(&v);
    CHECK(!v.grabbed && ws_visible(&v) && v.shown_image == IMG_GUEST,
          "on the strip: the strip's cursor, QEMU sets no arrow over it");
    strip_down(&v);
    CHECK(v.grabbed && !ws_visible(&v) && v.shown_image == IMG_EMPTY,
          "back over the VM: hidden, and the empty image reached the screen");
    background_show(&v);
    CHECK(!mac_arrow_seen(&v), "a show after the strip round trip shows nothing");
    leave(&v);
    CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW, "leaving anywhere else: the arrow");
}

static void test_shown_cases_keep_arrow(void)
{
    Vm v = vm(false);

    open_under_pointer(&v);
    become_key(&v);
    tablet_up(&v);
    motion(&v);
    CHECK(v.grabbed && ws_visible(&v) && v.shown_image == IMG_ARROW && !v.qemu_set,
          "booting (no hello yet): the arrow, QEMU sets no image");
    hello(&v);
    CHECK(!ws_visible(&v) && v.appkit == IMG_EMPTY, "hello: hidden, the empty image");
    resign_key(&v);         /* Finder in front */
    CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW && !v.qemu_set, "Finder in front: the arrow again");
    become_key(&v);
    CHECK(!ws_visible(&v) && v.appkit == IMG_EMPTY, "back: hidden and empty again");
    guest_reset(&v);
    CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW, "a guest reset: the arrow until its next hello");
    leave(&v);
    CHECK(ws_visible(&v) && v.shown_image == IMG_ARROW, "the pointer left the VM: the arrow");
}

int main(void)
{
    test_windowed_start();
    test_windowed_start_motion_during_boot();
    test_full_screen_start();
    test_full_screen_pointer_elsewhere();
    test_reboot();
    test_key_again();
    test_not_in_front();
    test_let_go();
    test_buttons();
    test_click_to_front();
    test_relative();
    test_show_cursor_on();
    test_hello_repeats();
    test_hello_takes();
    test_image_rules();
    test_background_show();
    test_macos_arrow();
    test_appkit_arrow();
    test_show_at_rest();
    test_strip();
    test_shown_cases_keep_arrow();
    if (failures) {
        printf("test-pointer-start: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test-pointer-start: all %d checks ok\n", checks);
    return 0;
}
