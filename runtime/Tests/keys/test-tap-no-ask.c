/*
 * No macOS prompt for QEMU's full-grab tap at the VM's start
 * (pingouin-cocoa-tap-no-ask.patch): pingouin_tap_allowed() from the patch (or
 * the patched ui/cocoa.m) as tap-allowed.inc, with made-up answers for
 * "control the computer". Allowed: the tap is made, nothing logged. Not
 * allowed: no tap (so no CGEventTapCreate and no prompt), one line saying why.
 * No QEMU, no tap, no permissions.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool post;
static int asked, logged;
static char last[512];

static bool fakePreflight(void) { asked++; return post; }
#define CGPreflightPostEventAccess fakePreflight
#define info_report(...) (logged++, snprintf(last, sizeof(last), __VA_ARGS__))

#include "tap-allowed.inc"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        fails++;
    }
}

int main(void)
{
    post = true; asked = logged = 0;
    check(pingouin_tap_allowed() && asked == 1 && logged == 0,
          "allowed: the tap is made, nothing in qemu.log");

    post = false; asked = logged = 0; last[0] = 0;
    check(!pingouin_tap_allowed() && asked == 1,
          "not allowed: no tap (CGEventTapCreate, and with it macOS's prompt, never runs)");
    check(logged == 1 && strstr(last, "not allowed to control the computer") &&
          strstr(last, "no macOS prompt"),
          "not allowed: qemu.log says why, once");
    printf("test-tap-no-ask: %s\n", fails ? "FAILED" : "the tap is made only when already allowed");
    return fails ? 1 : 0;
}
