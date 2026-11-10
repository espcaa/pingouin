/*
 * Every scale in FullPanel (#355, pingouin-fullpanel-scale.h): the guest's size
 * for each notched MacBook's whole panel and each of Omarchy's presets, a few
 * pixels more than the panel, which the scale fits. test-fullpanel-scale.sh
 * builds it against the patch. Reference: PR #355's tests (@brianmerchant).
 */
#include <stdio.h>
#include <string.h>
#include "pingouin-fullpanel-scale.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *name; uint32_t w, h; } Panel;
/* The whole panel (camera housing strip included) at macOS's default size, in pixels. */
static const Panel panels[] = {
    { "13-inch MacBook Air (1470x956 pt)", 2940, 1912 },
    { "15-inch MacBook Air (1710x1112 pt)", 3420, 2224 },
    { "14-inch MacBook Pro (1512x982 pt)", 3024, 1964 },
    { "16-inch MacBook Pro (1728x1117 pt)", 3456, 2234 },
};
/* Omarchy's scale presets (its display panel, omarchy-hyprland-monitor-scaling). */
static const double presets[] = { 1, 1.25, 1.6, 2, 3, 4 };

/* The expected sizes: one row per panel, one column per preset. */
static const uint32_t want[4][6][2] = {
    { { 2940, 1912 }, { 2940, 1915 }, { 2944, 1912 }, { 2940, 1912 }, { 2940, 1914 }, { 2940, 1912 } },
    { { 3420, 2224 }, { 3420, 2225 }, { 3424, 2224 }, { 3420, 2224 }, { 3420, 2226 }, { 3420, 2224 } },
    { { 3024, 1964 }, { 3025, 1965 }, { 3024, 1968 }, { 3024, 1964 }, { 3024, 1965 }, { 3024, 1964 } },
    { { 3456, 2234 }, { 3460, 2235 }, { 3456, 2240 }, { 3456, 2234 }, { 3456, 2235 }, { 3456, 2236 } },
};

int main(void)
{
    for (int i = 0; i < 4; i++) {
        const Panel *p = &panels[i];
        int fit_before = 0;
        for (int j = 0; j < 6; j++) {
            uint32_t k = pingouin_fp_scale_k(presets[j]), gw, gh;
            pingouin_fp_scale_size(p->w, p->h, k, &gw, &gh);
            fit_before += pingouin_fp_scale_fits(p->w, p->h, k);
            CHECK(gw == want[i][j][0] && gh == want[i][j][1], "%s, scale %.2f: %ux%u, want %ux%u",
                  p->name, presets[j], gw, gh, want[i][j][0], want[i][j][1]);
            CHECK(pingouin_fp_scale_fits(gw, gh, k), "%s, scale %.2f: %ux%u fits it", p->name, presets[j], gw, gh);
            CHECK(gw >= p->w && gh >= p->h && gw - p->w < PINGOUIN_FP_SCALE_GROW_MAX && gh - p->h < PINGOUIN_FP_SCALE_GROW_MAX,
                  "%s, scale %.2f: never smaller, a few pixels more at most (+%u, +%u)", p->name, presets[j],
                  gw - p->w, gh - p->h);
        }
        CHECK(fit_before < 6, "%s: the whole panel alone does not fit every preset (%d of 6)", p->name, fit_before);
        printf("ok   %s: every preset fits a size at most 7 pixels larger (the panel alone: %d of 6)\n", p->name, fit_before);
    }
    /* What the guest may send. */
    CHECK(pingouin_fp_scale_k(1.6) == 192 && pingouin_fp_scale_k(1.666667) == 200 && pingouin_fp_scale_k(1.25) == 150,
          "scales in 1/120: 1.6 -> 192, Omarchy's clean 1.666667 -> 200, 1.25 -> 150");
    CHECK(pingouin_fp_scale_k(1) == 120 && pingouin_fp_scale_k(4) == 480, "1 and 4 are in");
    CHECK(!pingouin_fp_scale_k(0.5) && !pingouin_fp_scale_k(4.5) && !pingouin_fp_scale_k(0) && !pingouin_fp_scale_k(-2) &&
          !pingouin_fp_scale_k(1e300) && !pingouin_fp_scale_k(0.0 / 0.0), "outside 1-4, not a number: none");
    uint32_t gw, gh;
    pingouin_fp_scale_size(2940, 1912, 0, &gw, &gh);
    CHECK(gw == 2940 && gh == 1912, "scale not known: the panel's own size");
    pingouin_fp_scale_size(2940, 1912, pingouin_fp_scale_k(1.8), &gw, &gh);   /* 216: a step of 9 */
    CHECK(gw == 2940 && gh == 1912, "a scale whose sizes are 9 pixels apart: the panel's own size");
    pingouin_fp_scale_size(2940, 1912, pingouin_fp_scale_k(1.666667), &gw, &gh);
    CHECK(gw == 2940 && gh == 1915 && pingouin_fp_scale_fits(gw, gh, 200), "Omarchy's clean 1.67 (from another size): 2940x1915");
    if (fails) {
        printf("test-fullpanel-scale: %d of %d checks failed\n", fails, checks);
        return 1;
    }
    printf("test-fullpanel-scale: %d checks passed\n", checks);
    return 0;
}
