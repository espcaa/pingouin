/*
 * The rules of ui/pingouin-display-settle.h (#428): one update at a time after
 * the displays settle, none while locked, 1 x 1 / inactive displays do not
 * count, full-screen tries per display. test-display-settle.sh builds it.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "pingouin-display-settle.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A run loop of one timer: changes at given times, updates counted. */
typedef struct Sim {
    PingouinDsTimer t;
    double timer_at;     /* < 0: no timer */
    int updates;
    double last_update;
    int changes_inside;  /* changes made by an update itself */
} Sim;

static void sim_arm(Sim *s, bool arm, double at)
{
    if (arm) {
        assert(s->timer_at < 0);   /* one timer at a time */
        s->timer_at = at;
    }
}

static void sim_change(Sim *s, double now)
{
    double at;
    sim_arm(s, pingouin_ds_ask(&s->t, now, PINGOUIN_DS_SETTLE_S, &at), at);
}

/* Runs the timer up to time end. */
static void sim_until(Sim *s, double end)
{
    while (s->timer_at >= 0 && s->timer_at <= end) {
        double now = s->timer_at, at;
        s->timer_at = -1;
        switch (pingouin_ds_fire(&s->t, now, &at)) {
        case PINGOUIN_DS_REARM:
            sim_arm(s, true, at);
            break;
        case PINGOUIN_DS_SKIP:
            break;
        case PINGOUIN_DS_RUN:
            s->updates++;
            s->last_update = now;
            for (int i = 0; i < s->changes_inside; i++) {
                sim_change(s, now);   /* e.g. a window it moved changed screen */
            }
            sim_arm(s, pingouin_ds_done(&s->t, now, 0, &at), at);
            break;
        }
    }
}

static void sim_init(Sim *s)
{
    memset(s, 0, sizeof(*s));
    s->timer_at = -1;
}

static void test_burst(void)
{
    Sim s;

    /* The report's burst: a screen change every 10 ms for 2.5 s -> one update. */
    sim_init(&s);
    for (int i = 0; i < 250; i++) {
        sim_change(&s, i * 0.01);
        sim_until(&s, i * 0.01);
    }
    sim_until(&s, 100);
    CHECK(s.updates == 1, "a 2.5 s burst ran %d updates", s.updates);
    CHECK(s.last_update >= 2.49 + PINGOUIN_DS_SETTLE_S - 0.01 && s.last_update <= 3.01,
          "the update ran at %.2f s", s.last_update);

    /* A longer burst still updates (after at most PINGOUIN_DS_MAX_WAIT_S). */
    sim_init(&s);
    for (int i = 0; i < 1000; i++) {
        sim_change(&s, i * 0.01);
        sim_until(&s, i * 0.01);
    }
    sim_until(&s, 100);
    CHECK(s.updates >= 3 && s.updates <= 5, "a 10 s burst ran %d updates", s.updates);

    /* One change: one update, PINGOUIN_DS_SETTLE_S later. */
    sim_init(&s);
    sim_change(&s, 5);
    sim_until(&s, 100);
    CHECK(s.updates == 1 && s.last_update > 5.49 && s.last_update < 5.51,
          "one change: %d updates, at %.2f", s.updates, s.last_update);
}

static void test_reentry(void)
{
    Sim s;

    /* Every update changes something itself: one more after it, not a loop inside. */
    sim_init(&s);
    s.changes_inside = 3;
    sim_change(&s, 0);
    sim_until(&s, 10);
    CHECK(s.updates >= 18 && s.updates <= 21,
          "updates that cause changes: %d in 10 s (one per %.1f s expected)",
          s.updates, PINGOUIN_DS_SETTLE_S);
    s.changes_inside = 0;
    sim_until(&s, 100);
    CHECK(s.timer_at < 0, "a timer is left");

    /* pingouin_ds_ask during an update never asks for a timer. */
    PingouinDsTimer t = { 0 };
    double at;
    assert(pingouin_ds_ask(&t, 0, PINGOUIN_DS_SETTLE_S, &at));
    CHECK(pingouin_ds_fire(&t, at, &at) == PINGOUIN_DS_RUN, "the update does not run");
    CHECK(!pingouin_ds_ask(&t, 1, PINGOUIN_DS_SETTLE_S, &at), "a timer inside the update");
    CHECK(pingouin_ds_done(&t, 1, 0, &at) && at > 1.49 && at < 1.51, "no update after it");
}

static void test_locked(void)
{
    Sim s;
    double at;

    /* Lock, 300 changes (the display unplugged, Spaces moved), unlock. */
    sim_init(&s);
    sim_arm(&s, pingouin_ds_lock(&s.t, true, 0, &at), at);
    for (int i = 0; i < 300; i++) {
        sim_change(&s, 1 + i * 0.01);
        sim_until(&s, 1 + i * 0.01);
    }
    sim_until(&s, 60);
    CHECK(s.updates == 0, "%d updates while locked", s.updates);
    sim_arm(&s, pingouin_ds_lock(&s.t, false, 60, &at), at);
    sim_until(&s, 100);
    CHECK(s.updates == 1 && s.last_update > 60.99 && s.last_update < 61.01,
          "after unlock: %d updates, at %.2f", s.updates, s.last_update);

    /* A timer armed before the lock fires while locked: nothing; unlock: one. */
    sim_init(&s);
    sim_change(&s, 0);
    sim_arm(&s, pingouin_ds_lock(&s.t, true, 0.1, &at), at);
    sim_until(&s, 30);
    CHECK(s.updates == 0, "%d updates while locked (timer from before)", s.updates);
    sim_arm(&s, pingouin_ds_lock(&s.t, false, 30, &at), at);
    sim_until(&s, 100);
    CHECK(s.updates == 1, "after unlock: %d updates", s.updates);

    /* Unlocked twice: no second update. */
    sim_arm(&s, pingouin_ds_lock(&s.t, false, 200, &at), at);
    sim_until(&s, 300);
    CHECK(s.updates == 1, "an unlock while unlocked ran an update");
}

static void test_counts(void)
{
    CHECK(!pingouin_ds_counts(false, false, 1, 1), "inactive 1 x 1 counts");
    CHECK(!pingouin_ds_counts(true, false, 1, 1), "1 x 1 counts");
    CHECK(!pingouin_ds_counts(true, false, 0, 0), "0 x 0 counts");
    CHECK(!pingouin_ds_counts(false, false, 3440, 1440), "inactive counts");
    CHECK(!pingouin_ds_counts(true, true, 3440, 1440), "asleep counts");
    CHECK(pingouin_ds_counts(true, false, 3440, 1440), "the LG does not count");
    CHECK(pingouin_ds_counts(true, false, 1470, 956), "a 14\" MacBook Pro does not count");

    /* Inactive for 2.6 s (the report's wake): no window until it counted 2 s again. */
    PingouinDsDisplays all = { 0 };
    PingouinDsDisplay *d = pingouin_ds_display(&all, 2, 0);
    CHECK(pingouin_ds_look(d, true, 0) > 1.99, "a new display gets a window at once");
    CHECK(pingouin_ds_look(d, true, 2) == 0, "a display that counted 2 s gets none");
    CHECK(pingouin_ds_look(d, false, 14.5) < 0, "an inactive display keeps its window");
    double left = pingouin_ds_look(d, true, 17.1);
    CHECK(left > 1.99 && left < 2.01, "back: %.2f s to wait", left);
    CHECK(pingouin_ds_look(d, true, 18) > 0, "a window 0.9 s after it came back");
    CHECK(pingouin_ds_look(d, true, 19.2) == 0, "no window 2.1 s after it came back");

    /* Unplugged for 2.5 s (not listed, so never looked at): it waits again when back. */
    uint32_t none[1] = { 1 }, both[2] = { 1, 2 };
    pingouin_ds_listed(&all, both, 2);
    CHECK(pingouin_ds_look(d, true, 30) == 0, "a listed display lost its time");
    pingouin_ds_listed(&all, none, 1);
    left = pingouin_ds_look(pingouin_ds_display(&all, 2, 32.5), true, 32.5);
    CHECK(left > 1.99, "plugged in again after 2.5 s: a window at once (%.2f s to wait)", left);
}

/*
 * The report's loop: the display's window made again and again, each time
 * in full screen (a new Space) that holds well under PINGOUIN_DS_HOLD_S.
 * With the tries per window (3.1.14) every new window started from 0.
 */
static int loop_tries(bool per_display, double held, double every, int rounds)
{
    PingouinDsDisplays all = { 0 };
    int toggles = 0, old_tries = 0;
    double wait;

    for (int r = 0; r < rounds; r++) {
        double now = r * every;
        PingouinDsDisplay *d = pingouin_ds_display(&all, 3, now);
        if (!per_display) {
            old_tries = 0;   /* pingouin_head_on: full_tries = 0 */
            if (++old_tries <= 5) {
                toggles++;
            }
            continue;
        }
        if (pingouin_ds_may_try(d, now, &wait)) {
            pingouin_ds_tried(d, now);
            toggles++;
            pingouin_ds_full(d, true, now + 0.5);
        }
        pingouin_ds_full(d, false, now + held);   /* its window goes */
    }
    return toggles;
}

static void test_tries(void)
{
    int t;

    t = loop_tries(false, 0.4, 0.5, 89);
    CHECK(t == 89, "old rules: %d toggles in the loop (the check is wrong)", t);
    t = loop_tries(true, 0.4, 0.5, 89);
    CHECK(t <= PINGOUIN_DS_TRIES, "%d full-screen toggles for 89 windows of one display", t);
    t = loop_tries(true, 0.4, 0.5, 10000);
    CHECK(t <= PINGOUIN_DS_TRIES, "%d full-screen toggles for 10000 windows", t);

    /* The pause between tries doubles. */
    PingouinDsDisplays all = { 0 };
    PingouinDsDisplay *d = pingouin_ds_display(&all, 7, 0);
    double wait, now = 0;
    for (int i = 0; i < PINGOUIN_DS_TRIES; i++) {
        CHECK(pingouin_ds_may_try(d, now, &wait), "try %d refused at %.1f", i + 1, now);
        pingouin_ds_tried(d, now);
        CHECK(!pingouin_ds_may_try(d, now + 0.01, &wait) &&
              (i + 1 == PINGOUIN_DS_TRIES ? wait < 0 :
               wait > PINGOUIN_DS_BACKOFF_S * (1 << i) - 0.02),
              "try %d: wait %.2f", i + 2, wait);
        now += PINGOUIN_DS_BACKOFF_S * (1 << i);
    }
    CHECK(!pingouin_ds_may_try(d, 1e6, &wait) && wait < 0, "tries left after %d", PINGOUIN_DS_TRIES);

    /* A full screen that held starts over (a display plugged in again later). */
    d = pingouin_ds_display(&all, 8, 0);
    pingouin_ds_tried(d, 0);
    pingouin_ds_full(d, true, 1);
    pingouin_ds_full(d, false, 3600);
    CHECK(d->tries == 0 && pingouin_ds_may_try(d, 3600, &wait), "held for an hour: %d tries", d->tries);
    /* Gone before it held: counted. */
    pingouin_ds_tried(d, 4000);
    pingouin_ds_full(d, true, 4001);
    pingouin_ds_full(d, false, 4003);
    CHECK(d->tries == 1, "gone after 2 s: %d tries", d->tries);
    /* Never in full screen (refused): counted. */
    pingouin_ds_full(d, false, 9000);
    CHECK(d->tries == 1, "not in full screen: %d tries", d->tries);

    /* Per display: another display has its own tries. */
    PingouinDsDisplay *e = pingouin_ds_display(&all, 9, 0);
    CHECK(e->tries == 0, "a new display starts with %d tries", e->tries);
}

static void test_key(void)
{
    uint32_t lg = pingouin_ds_key(0x1e6d, 0x5b11, 0x1234, 2);

    CHECK(lg == pingouin_ds_key(0x1e6d, 0x5b11, 0x1234, 3), "a new display id is another monitor");
    CHECK(lg != pingouin_ds_key(0x1e6d, 0x5b11, 0x1235, 2), "another serial number is the same monitor");
    CHECK(lg != pingouin_ds_key(0x1e6d, 0x5b12, 0x1234, 2), "another model is the same monitor");
    CHECK(pingouin_ds_key(0, 0, 0, 7) == 7, "a display without a vendor is not known by its id");
    CHECK(lg != 0, "a key of 0 (a free entry)");
}

static void test_table(void)
{
    PingouinDsDisplays all = { 0 };

    for (uint32_t id = 1; id <= PINGOUIN_DS_MAX; id++) {
        PingouinDsDisplay *d = pingouin_ds_display(&all, id, id);
        d->tries = (int)id;
    }
    CHECK(pingouin_ds_display(&all, 5, 100)->tries == 5, "display 5 lost its entry");
    /* A 17th display takes the oldest entry (1), not 5 (seen at 100). */
    PingouinDsDisplay *n = pingouin_ds_display(&all, 99, 101);
    CHECK(n->tries == 0 && n->id == 99, "the new display has an old entry");
    CHECK(pingouin_ds_display(&all, 5, 102)->tries == 5, "display 5 was replaced");
    CHECK(pingouin_ds_display(&all, 1, 103)->tries == 0, "display 1 kept its entry");
}

int main(void)
{
    test_burst();
    test_reentry();
    test_locked();
    test_counts();
    test_tries();
    test_key();
    test_table();
    if (failures) {
        printf("test-display-settle: %d failed\n", failures);
        return 1;
    }
    printf("test-display-settle: ok\n");
    return 0;
}
