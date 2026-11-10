/*
 * The guest's modifier keys follow each event's flags (#412,
 * pingouin-cocoa-modifier-state-logic.patch). Runs the report's sequences, every
 * pair and side, events without key bits, and a seeded random run against a
 * model keyboard through ui/pingouin-modifiers.h, and through QEMU's old rules
 * (flip a key on each flagsChanged), which must fail the report's sequences.
 */
#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pingouin-modifiers.h"

enum { FC, KD, KU, PTR };   /* flagsChanged, key down, key up, pointer/scroll */

typedef struct {
    int type, code;
    uint64_t flags;
} Ev;

/* The guest: 8 modifier keys (pair * 2 + side) and one other key. */
typedef struct {
    bool mod[8], key;
    char log[512];
    bool key_ok;        /* every key down saw the modifiers the event said */
} Guest;

static bool captured = true, left_cmd = true, old_rules;
static int fails;

static void out(Guest *g, int k, bool down)
{
    static const char *names[] = { "LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "LMeta", "RMeta" };
    size_t n = strlen(g->log);
    snprintf(g->log + n, sizeof(g->log) - n, "%s%s%c", n ? " " : "", k < 0 ? "key" : names[k], down ? 'v' : '^');
}

static bool may(int p, int side)
{
    return p != PINGOUIN_MOD_COMMAND || (captured && (side || left_cmd));
}

/* QEMU's rules before #412: up when the flag is clear, flip on flagsChanged. */
static void old_step(Guest *g, const Ev *e)
{
    for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
        if (!(e->flags & pingouin_mod_bits[p].flag)) {
            for (int s = 0; s < 2; s++) {
                if (g->mod[p * 2 + s]) {
                    g->mod[p * 2 + s] = false;
                    out(g, p * 2 + s, false);
                }
            }
        }
    }
    if (e->type != FC) {
        return;
    }
    for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
        for (int s = 0; s < 2; s++) {
            int code = s ? pingouin_mod_bits[p].right_code : pingouin_mod_bits[p].left_code;
            if (e->code == code && (e->flags & pingouin_mod_bits[p].flag) && may(p, s)) {
                g->mod[p * 2 + s] = !g->mod[p * 2 + s];
                out(g, p * 2 + s, g->mod[p * 2 + s]);
            }
        }
    }
}

/* pingouinSyncModifiers in ui/cocoa.m: releases first, then presses. */
static void new_step(Guest *g, const Ev *e)
{
    PingouinModKeys now[PINGOUIN_MOD_PAIRS], next[PINGOUIN_MOD_PAIRS];
    PingouinModEvent kind = e->type == KD || e->type == FC ? PINGOUIN_MOD_EV_KEY : PINGOUIN_MOD_EV_INPUT;

    for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
        now[p] = (PingouinModKeys){ g->mod[p * 2], g->mod[p * 2 + 1] };
        next[p] = pingouin_mod_next(p, now[p], kind, e->flags, e->type == FC ? e->code : -1,
                                  may(p, 0), may(p, 1));
    }
    for (int down = 0; down < 2; down++) {
        for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
            bool n[2] = { next[p].left, next[p].right }, o[2] = { now[p].left, now[p].right };
            for (int s = 0; s < 2; s++) {
                if (o[s] != n[s] && n[s] == down) {
                    g->mod[p * 2 + s] = n[s];
                    out(g, p * 2 + s, n[s]);
                }
            }
        }
    }
}

static void step(Guest *g, const Ev *e)
{
    (old_rules ? old_step : new_step)(g, e);
    if (e->type == KD) {
        for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
            bool want = (e->flags & pingouin_mod_bits[p].flag) && (may(p, 0) || may(p, 1));
            if ((g->mod[p * 2] || g->mod[p * 2 + 1]) != want) {
                g->key_ok = false;
            }
        }
        g->key = true;
        out(g, -1, true);
    } else if (e->type == KU) {
        g->key = false;
    }
}

static const char *run(const Ev *evs, int n, Guest *g)
{
    memset(g, 0, sizeof(*g));
    g->key_ok = true;
    for (int i = 0; i < n; i++) {
        step(g, &evs[i]);
    }
    return g->log;
}

#define SH NX_SHIFTMASK
#define CT NX_CONTROLMASK
#define AL NX_ALTERNATEMASK
#define CM NX_COMMANDMASK
#define LS NX_DEVICELSHIFTKEYMASK
#define RS NX_DEVICERSHIFTKEYMASK
#define K2 kVK_ANSI_2

static void expect(const char *name, const Ev *evs, int n, const char *want)
{
    Guest g;
    const char *got = run(evs, n, &g);
    if (strcmp(got, want) != 0) {
        printf("FAIL %s\n  got:  %s\n  want: %s\n", name, got, want);
        fails++;
    } else if (!old_rules) {
        printf("ok   %s: %s\n", name, got);
    }
}
#define EXPECT(name, want, ...) do { const Ev e_[] = { __VA_ARGS__ }; \
    expect(name, e_, (int)(sizeof(e_) / sizeof(e_[0])), want); } while (0)

/* The report's two ways (#412): QEMU's old rules type 2 for Shift+2. */
static int report_cases(bool check_old)
{
    static const Ev twice[] = {
        { FC, kVK_Shift, SH | LS }, { FC, kVK_Shift, SH | LS },
        { KD, K2, SH | LS }, { KU, K2, SH | LS }, { FC, kVK_Shift, 0 },
    };
    static const Ev flagless[] = {
        { FC, kVK_Shift, SH | LS }, { PTR, 0, 0 },
        { KD, K2, SH | LS }, { KU, K2, SH | LS }, { FC, kVK_Shift, 0 },
    };
    static const Ev twice_nobits[] = {
        { FC, kVK_Shift, SH }, { FC, kVK_Shift, SH },
        { KD, K2, SH }, { KU, K2, SH }, { FC, kVK_Shift, 0 },
    };
    const Ev *cases[] = { twice, flagless, twice_nobits };
    const char *names[] = { "flagsChanged twice", "an event without the flag after it",
                            "flagsChanged twice, no key bits" };
    int wrong = 0;
    for (int i = 0; i < 3; i++) {
        Guest g;
        run(cases[i], 5, &g);
        if (check_old) {
            printf("old rules, %s: %s -> %s\n", names[i], g.log, g.key_ok ? "Shift+2 typed @" : "Shift+2 typed 2");
        }
        wrong += !g.key_ok;
    }
    return wrong;
}

/*
 * A model keyboard: random presses and releases of the 8 modifier keys and
 * one other key, each event with the flags and key bits macOS gives it; some
 * flagsChanged come twice, some pointer events carry no flags (another Mac's
 * keyboard). At every key down the guest must hold what the flags say, and
 * with no flag-less events in the run, change only when a key changes.
 */
static int random_run(unsigned seed, int rounds, bool flagless, bool *quiet_ok)
{
    static const uint64_t bit[8] = { LS, RS, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK,
                                     NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK,
                                     NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK };
    static const int code[8] = { kVK_Shift, kVK_RightShift, kVK_Control, kVK_RightControl,
                                 kVK_Option, kVK_RightOption, kVK_Command, kVK_RightCommand };
    static const uint64_t pairflag[4] = { SH, CT, AL, CM };
    int bad = 0;
    *quiet_ok = true;
    srandom(seed);
    for (int r = 0; r < rounds; r++) {
        bool held[8] = { 0 };
        Guest g;
        int changes = 0, guest_changes = 0;
        memset(&g, 0, sizeof(g));
        g.key_ok = true;
        for (int i = 0; i < 40; i++) {
            uint64_t flags = 0;
            int k = (int)(random() % 10);
            Ev e = { 0 };
            if (k < 8) {
                held[k] = !held[k];
                changes++;
                e.type = FC;
                e.code = code[k];
            } else if (k == 8) {
                e.type = KD;
                e.code = K2;
            } else {
                e.type = flagless ? PTR : KU;
                e.code = K2;
            }
            for (int j = 0; j < 8; j++) {
                if (held[j]) {
                    flags |= pairflag[j / 2] | bit[j];
                }
            }
            e.flags = e.type == PTR ? 0 : flags;
            size_t before = strlen(g.log);
            step(&g, &e);
            if (e.type == FC && random() % 3 == 0) {
                step(&g, &e);   /* the same change again */
            }
            for (const char *c = g.log + before; *c; c++) {
                guest_changes += *c == 'v' || *c == '^';
            }
            if (e.type == KD) {
                guest_changes--;    /* the key itself */
            }
            if (strlen(g.log) > 400) {
                g.log[0] = 0;
            }
        }
        bad += !g.key_ok;
        if (!flagless && guest_changes > changes) {
            *quiet_ok = false;
        }
    }
    return bad;
}

int main(void)
{
    /* The table matches macOS's own values. */
    static const struct { uint64_t flag, l, r; int lc, rc; } mac[] = {
        { NX_SHIFTMASK, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, kVK_Shift, kVK_RightShift },
        { NX_CONTROLMASK, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, kVK_Control, kVK_RightControl },
        { NX_ALTERNATEMASK, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, kVK_Option, kVK_RightOption },
        { NX_COMMANDMASK, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, kVK_Command, kVK_RightCommand },
    };
    for (int p = 0; p < PINGOUIN_MOD_PAIRS; p++) {
        const PingouinModBits *b = &pingouin_mod_bits[p];
        if (b->flag != mac[p].flag || b->left_bit != mac[p].l || b->right_bit != mac[p].r ||
            b->left_code != mac[p].lc || b->right_code != mac[p].rc) {
            printf("FAIL pair %d: bits or key codes differ from macOS's\n", p);
            fails++;
        }
    }

    EXPECT("Shift+2, the Mac's own keyboard", "LShiftv keyv LShift^",
           { FC, kVK_Shift, SH | LS }, { KD, K2, SH | LS }, { KU, K2, SH | LS }, { FC, kVK_Shift, 0 });
    EXPECT("#412: flagsChanged twice (press and release)", "LShiftv keyv LShift^",
           { FC, kVK_Shift, SH | LS }, { FC, kVK_Shift, SH | LS }, { KD, K2, SH | LS }, { KU, K2, SH | LS },
           { FC, kVK_Shift, 0 }, { FC, kVK_Shift, 0 });
    EXPECT("#412: an event without the flag right after", "LShiftv LShift^ LShiftv keyv LShift^",
           { FC, kVK_Shift, SH | LS }, { PTR, 0, 0 }, { KD, K2, SH | LS }, { KU, K2, SH | LS },
           { FC, kVK_Shift, 0 });
    EXPECT("#412: both, no key bits", "LShiftv LShift^ LShiftv keyv LShift^",
           { FC, kVK_Shift, SH }, { FC, kVK_Shift, SH }, { PTR, 0, 0 }, { KD, K2, SH },
           { KU, K2, SH }, { FC, kVK_Shift, 0 });
    EXPECT("right Shift", "RShiftv keyv RShift^",
           { FC, kVK_RightShift, SH | RS }, { FC, kVK_RightShift, SH | RS }, { KD, K2, SH | RS },
           { KU, K2, SH | RS }, { FC, kVK_RightShift, 0 });
    EXPECT("both Shifts, right let go first", "LShiftv RShiftv RShift^ keyv LShift^",
           { FC, kVK_Shift, SH | LS }, { FC, kVK_RightShift, SH | LS | RS }, { FC, kVK_RightShift, SH | LS },
           { KD, K2, SH | LS }, { FC, kVK_Shift, 0 });
    EXPECT("both Shifts, no key bits", "LShiftv RShiftv RShift^ keyv LShift^",
           { FC, kVK_Shift, SH }, { FC, kVK_RightShift, SH }, { FC, kVK_RightShift, SH },
           { KD, K2, SH }, { FC, kVK_Shift, 0 });
    EXPECT("left to right in one event", "LShiftv LShift^ RShiftv keyv RShift^",
           { FC, kVK_Shift, SH | LS }, { KD, K2, SH | RS }, { FC, kVK_RightShift, 0 });
    EXPECT("Control left and right", "LCtrlv keyv LCtrl^ RCtrlv RCtrl^",
           { FC, kVK_Control, CT | NX_DEVICELCTLKEYMASK }, { KD, kVK_ANSI_C, CT | NX_DEVICELCTLKEYMASK },
           { FC, kVK_Control, 0 }, { FC, kVK_RightControl, CT | NX_DEVICERCTLKEYMASK },
           { FC, kVK_RightControl, CT | NX_DEVICERCTLKEYMASK }, { FC, kVK_RightControl, 0 });
    EXPECT("Option left and right", "LAltv LAlt^ RAltv keyv RAlt^",
           { FC, kVK_Option, AL | NX_DEVICELALTKEYMASK }, { FC, kVK_Option, 0 },
           { FC, kVK_RightOption, AL | NX_DEVICERALTKEYMASK }, { KD, K2, AL | NX_DEVICERALTKEYMASK },
           { FC, kVK_RightOption, 0 });
    EXPECT("Command left and right (the VM has the keyboard)", "LMetav keyv LMeta^ RMetav RMeta^",
           { FC, kVK_Command, CM | NX_DEVICELCMDKEYMASK }, { KD, kVK_ANSI_T, CM | NX_DEVICELCMDKEYMASK },
           { FC, kVK_Command, 0 }, { FC, kVK_RightCommand, CM | NX_DEVICERCMDKEYMASK },
           { FC, kVK_RightCommand, 0 });
    EXPECT("held key repeats: Shift once", "LShiftv keyv keyv keyv keyv LShift^",
           { FC, kVK_Shift, SH | LS }, { KD, K2, SH | LS }, { KD, K2, SH | LS }, { KD, K2, SH | LS },
           { FC, kVK_Shift, SH | LS }, { KD, K2, SH | LS }, { KU, K2, SH | LS }, { FC, kVK_Shift, 0 });
    EXPECT("a pointer with Shift presses nothing", "keyv",
           { PTR, 0, SH | LS }, { KU, K2, SH | LS }, { KD, K2, 0 });
    EXPECT("a pointer with the other side releases", "LShiftv LShift^",
           { FC, kVK_Shift, SH | LS }, { PTR, 0, SH | RS });
    EXPECT("Shift held while QEMU came to the front", "LShiftv keyv LShift^",
           { KD, K2, SH | LS }, { KU, K2, SH | LS }, { FC, kVK_Shift, 0 });
    captured = false;
    EXPECT("Command without the keyboard: not the VM's", "keyv",
           { FC, kVK_Command, CM | NX_DEVICELCMDKEYMASK }, { KD, kVK_ANSI_T, CM | NX_DEVICELCMDKEYMASK },
           { FC, kVK_Command, 0 });
    EXPECT("Shift without the keyboard: as before", "LShiftv LShift^",
           { FC, kVK_Shift, SH | LS }, { FC, kVK_Shift, 0 });
    captured = true;
    EXPECT("Command, keyboard taken while it is held: then released", "LMetav keyv LMeta^",
           { KD, kVK_ANSI_T, CM | NX_DEVICELCMDKEYMASK }, { FC, kVK_Command, 0 });
    left_cmd = false;
    EXPECT("left Command off (-display cocoa,left-command-key=off)", "RMetav RMeta^",
           { FC, kVK_Command, CM | NX_DEVICELCMDKEYMASK }, { FC, kVK_Command, 0 },
           { FC, kVK_RightCommand, CM | NX_DEVICERCMDKEYMASK }, { FC, kVK_RightCommand, 0 });
    left_cmd = true;

    bool quiet_ok;
    int bad = random_run(412, 20000, false, &quiet_ok);
    printf("%s random, flagsChanged sometimes twice: %d of 20000 runs with a key down seeing the wrong modifiers; "
           "%s\n", bad ? "FAIL" : "ok  ", bad, quiet_ok ? "no extra modifier changes" : "EXTRA modifier changes");
    fails += bad || !quiet_ok;
    bad = random_run(4120, 20000, true, &quiet_ok);
    printf("%s random, also pointer events without flags: %d of 20000 runs with a key down seeing the wrong modifiers\n",
           bad ? "FAIL" : "ok  ", bad);
    fails += bad != 0;

    /* The old rules fail the report's sequences, so the checks above catch them. */
    old_rules = true;
    int old_wrong = report_cases(true);
    int old_random = random_run(412, 20000, false, &quiet_ok);
    old_rules = false;
    if (old_wrong != 3 || old_random == 0) {
        printf("FAIL the old rules pass the report's sequences (%d of 3 wrong, %d random): the test misses #412\n",
               old_wrong, old_random);
        fails++;
    } else {
        printf("ok   old rules: Shift+2 typed 2 in 3 of 3 of the report's sequences, %d of 20000 random runs wrong\n",
               old_random);
    }
    if (report_cases(false) != 0) {
        printf("FAIL the new rules type 2 in a report sequence\n");
        fails++;
    }
    printf("%s\n", fails ? "test-modifier-state: FAIL" : "test-modifier-state: ok");
    return fails != 0;
}
