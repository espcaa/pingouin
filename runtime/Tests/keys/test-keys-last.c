/*
 * QEMU's full-grab tap and keyboard sharing apps (#350,
 * pingouin-cocoa-keys-last.patch): pingouin_keys_place() from the patch (or the
 * patched ui/cocoa.m) as keys-place.inc. PINGOUIN_KEYS_LAST=1 (Pingouin.app's
 * hidden shareKeyboard): at the end of macOS's event tap chain; anything
 * else: at its head, as before. No QEMU, no tap.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keys-place.inc"

static int fails;

static void expect(const char *value, CGEventTapPlacement want, const char *what)
{
    if (value) {
        setenv("PINGOUIN_KEYS_LAST", value, 1);
    } else {
        unsetenv("PINGOUIN_KEYS_LAST");
    }
    CGEventTapPlacement got = pingouin_keys_place();
    printf("%s %s\n", got == want ? "ok  " : "FAIL", what);
    if (got != want) {
        fails++;
    }
}

int main(void)
{
    expect(NULL, kCGHeadInsertEventTap, "not set: the head of the chain, as before");
    expect("1", kCGTailAppendEventTap, "PINGOUIN_KEYS_LAST=1 (shareKeyboard on): the end of the chain");
    expect("0", kCGHeadInsertEventTap, "PINGOUIN_KEYS_LAST=0: the head");
    expect("", kCGHeadInsertEventTap, "PINGOUIN_KEYS_LAST empty: the head");
    expect("yes", kCGHeadInsertEventTap, "PINGOUIN_KEYS_LAST=yes: the head (only 1 counts)");
    printf("test-keys-last: %s\n", fails ? "FAILED" : "the tap's place follows PINGOUIN_KEYS_LAST");
    return fails ? 1 : 0;
}
