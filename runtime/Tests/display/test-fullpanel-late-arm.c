/*
 * FullPanel armed late (ui/pingouin-fullpanel-late.h,
 * pingouin-cocoa-fullpanel-late-arm.patch, #716): the rules. The window's
 * state with a late entry (the lid closed at the start, then opened) is
 * replayed in test-fullpanel-retake.c.
 */
#include <stdio.h>
#include <string.h>

#include "pingouin-fullpanel-late.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    double w = 0, h = 0;

    /* A display with a housing and every part: the strip now (the main window or another). */
    CHECK(pingouin_fp_entry(PINGOUIN_FP_OK, PINGOUIN_FP_OK, true) == PINGOUIN_FP_ENTRY_ARM);
    CHECK(pingouin_fp_entry(PINGOUIN_FP_OK, PINGOUIN_FP_OK, false) == PINGOUIN_FP_ENTRY_ARM);
    /* The main window without a housing, every other part there: late. */
    CHECK(pingouin_fp_entry(PINGOUIN_FP_NO_NOTCH, PINGOUIN_FP_OK, true) == PINGOUIN_FP_ENTRY_LATE);
    /* The other displays' windows are made again per display: normal full screen, as before. */
    CHECK(pingouin_fp_entry(PINGOUIN_FP_NO_NOTCH, PINGOUIN_FP_OK, false) == PINGOUIN_FP_ENTRY_NORMAL);
    CHECK(pingouin_fp_entry_why(PINGOUIN_FP_NO_NOTCH, PINGOUIN_FP_OK, false) == PINGOUIN_FP_NO_NOTCH);
    /* Another part missing: never late; the line names that part for the main window. */
    for (PingouinFPFallback f = PINGOUIN_FP_MENU_BAR_SHOWN; f <= PINGOUIN_FP_NO_SKYLIGHT; f++) {
        CHECK(pingouin_fp_entry(PINGOUIN_FP_NO_NOTCH, f, true) == PINGOUIN_FP_ENTRY_NORMAL);
        CHECK(pingouin_fp_entry_why(PINGOUIN_FP_NO_NOTCH, f, true) == f);
        CHECK(pingouin_fp_entry_why(PINGOUIN_FP_NO_NOTCH, f, false) == PINGOUIN_FP_NO_NOTCH);
        /* ... and on a display with a housing, as before. */
        CHECK(pingouin_fp_entry(f, f, true) == PINGOUIN_FP_ENTRY_NORMAL);
        CHECK(pingouin_fp_entry_why(f, f, true) == f);
    }

    /* didEnterFullScreen: entered with the strip, or armed late; never unarmed. */
    CHECK(pingouin_fp_entered(true, true, false));
    CHECK(pingouin_fp_entered(true, false, true));
    CHECK(!pingouin_fp_entered(true, false, false));
    CHECK(!pingouin_fp_entered(false, true, true));
    CHECK(!pingouin_fp_entered(false, false, true));

    /* The panel this start's notch was measured on: "WxD" in points. */
    CHECK(pingouin_fp_panel("1728x1117", &w, &h) && w == 1728 && h == 1117);
    CHECK(pingouin_fp_panel("1512.0x982.0", &w, &h) && w == 1512 && h == 982);
    w = h = 7;
    CHECK(!pingouin_fp_panel(NULL, &w, &h) && !pingouin_fp_panel("", &w, &h));
    CHECK(!pingouin_fp_panel("1728", &w, &h) && !pingouin_fp_panel("1728x", &w, &h));
    CHECK(!pingouin_fp_panel("x1117", &w, &h) && !pingouin_fp_panel("1728x1117x", &w, &h));
    CHECK(!pingouin_fp_panel("1728x1117 ", &w, &h) && !pingouin_fp_panel("1728,1117", &w, &h));
    CHECK(!pingouin_fp_panel("0x0", &w, &h) && !pingouin_fp_panel("-1728x1117", &w, &h));
    CHECK(!pingouin_fp_panel("nanx1117", &w, &h) && !pingouin_fp_panel("1728xinf", &w, &h));
    CHECK(!pingouin_fp_panel("99x1117", &w, &h) && !pingouin_fp_panel("1728x30000", &w, &h));
    CHECK(w == 7 && h == 7);   /* untouched when not a size */

    /* A late full screen takes the strip only on that panel's size (half a point). */
    CHECK(pingouin_fp_late_fits(true, true, 1728, 1117, 1728, 1117));
    CHECK(pingouin_fp_late_fits(true, true, 1728, 1117, 1728.4, 1116.6));
    CHECK(!pingouin_fp_late_fits(true, true, 1728, 1117, 2056, 1329));
    CHECK(!pingouin_fp_late_fits(true, true, 1728, 1117, 1728, 1084));
    CHECK(!pingouin_fp_late_fits(true, true, 1728, 1117, 0, 0));
    /* Not late, or no size given (an older app): anything fits. */
    CHECK(pingouin_fp_late_fits(false, true, 1728, 1117, 2056, 1329));
    CHECK(pingouin_fp_late_fits(true, false, 0, 0, 2056, 1329));

    if (failures) {
        printf("test-fullpanel-late-arm: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ok   %d rule checks\n", checks);
    return 0;
}
