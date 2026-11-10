/*
 * Closing the window (pingouin-close.h, from pingouin-cocoa-close-logic.patch):
 * which setting counts and what it does. test-close.sh builds it against
 * the patch.
 */
#include <stdio.h>
#include "pingouin-close.h"

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
    check(pingouin_close_parse("shutdown") == PINGOUIN_CLOSE_SHUTDOWN, "shutdown: shut down");
    check(pingouin_close_parse("keepRunning") == PINGOUIN_CLOSE_KEEP, "keepRunning: keep running");
    check(pingouin_close_parse("ask") == PINGOUIN_CLOSE_ASK, "ask: ask");
    check(pingouin_close_parse(NULL) == PINGOUIN_CLOSE_SHUTDOWN, "not set: shut down (as before the setting)");
    check(pingouin_close_parse("") == PINGOUIN_CLOSE_SHUTDOWN && pingouin_close_parse("KeepRunning") == PINGOUIN_CLOSE_SHUTDOWN &&
          pingouin_close_parse("keep") == PINGOUIN_CLOSE_SHUTDOWN && pingouin_close_parse("Ask") == PINGOUIN_CLOSE_SHUTDOWN &&
          pingouin_close_parse("keepRunning ") == PINGOUIN_CLOSE_SHUTDOWN,
          "anything else (case, a typo, a space): shut down");
    check(pingouin_close_pick(true, "keepRunning", "shutdown") == PINGOUIN_CLOSE_KEEP,
          "QEMU as the Pingouin app: the app's setting now (changed while the VM runs) over the start's");
    check(pingouin_close_pick(true, NULL, "keepRunning") == PINGOUIN_CLOSE_SHUTDOWN,
          "... the setting taken back to the default while the VM runs: shut down");
    check(pingouin_close_pick(false, "ask", "keepRunning") == PINGOUIN_CLOSE_KEEP,
          "not the Pingouin app (a development build): PINGOUIN_CLOSE_ACTION from the start");
    check(pingouin_close_pick(false, NULL, NULL) == PINGOUIN_CLOSE_SHUTDOWN, "... nothing passed: shut down");
    check(!strcmp(pingouin_close_name(PINGOUIN_CLOSE_KEEP), "keepRunning") && !strcmp(pingouin_close_name(PINGOUIN_CLOSE_ASK), "ask") &&
          !strcmp(pingouin_close_name(PINGOUIN_CLOSE_SHUTDOWN), "shutdown"), "names as the setting's values");
    return fails ? 1 : 0;
}
