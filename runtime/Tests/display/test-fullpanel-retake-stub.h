/*
 * 3.2.10's decisions behind the API of ui/pingouin-fullpanel-retake.h, for
 * test-fullpanel-retake.sh --self-test: the user's rows must fail with them
 * (the strip stays below the notch). Not used by QEMU.
 */
#ifndef PINGOUIN_FULLPANEL_RETAKE_STUB_H
#define PINGOUIN_FULLPANEL_RETAKE_STUB_H

#include <stdbool.h>
#include <stddef.h>

#include "pingouin-fullpanel.h"

#define PINGOUIN_FP_RETAKE_TRIES 3
#define PINGOUIN_FP_RETAKE_CHECK_S 2.0
#define PINGOUIN_FP_BAND_SLACK 8.0
#define PINGOUIN_FP_RETAKE_REASK_S 0.5
#define PINGOUIN_FP_WHY_BAND_TALL "3.2.10 has no band"
/* 3.2.10's claim waited for -screenChanged:'s resume (!released). */
#define PINGOUIN_FP_CLAIMS_WAIT_FOR_RESUME 1

static inline bool pingouin_fp_inside(PingouinFPRect f, PingouinFPRect d)
{
    return f.x >= d.x - 0.5 && f.y >= d.y - 0.5 &&
           f.x + f.w <= d.x + d.w + 0.5 && f.y + f.h <= d.y + d.h + 0.5;
}

/* 3.2.10 had no tile shape. */
static inline bool pingouin_fp_tile(PingouinFPRect f, PingouinFPRect d)
{
    (void)f;
    (void)d;
    return false;
}

typedef enum {
    PINGOUIN_FP_FRAME_COVERS,
    PINGOUIN_FP_FRAME_DISPLACED,
    PINGOUIN_FP_FRAME_LOST,
    PINGOUIN_FP_FRAME_WAIT,
} PingouinFPFrameKind;

/* 3.2.10: pingouin_fp_lost(accepted, true, covers, display_unchanged). */
static inline PingouinFPFrameKind pingouin_fp_frame_kind(bool covers, bool inside, bool display_live,
                                                     bool same_display, bool display_changed,
                                                     bool tile)
{
    (void)inside;
    (void)display_live;
    (void)display_changed;
    (void)tile;
    if (covers) {
        return PINGOUIN_FP_FRAME_COVERS;
    }
    return pingouin_fp_lost(true, true, covers, same_display) ? PINGOUIN_FP_FRAME_LOST
                                                            : PINGOUIN_FP_FRAME_WAIT;
}

static inline bool pingouin_fp_lost_to_display_change(bool lost, double since, bool display_changed,
                                                    bool tile)
{
    (void)lost;
    (void)since;
    (void)display_changed;
    (void)tile;
    return false;
}

static inline bool pingouin_fp_claims(bool inside, bool armed, bool rejected, bool lost,
                                    bool menu_failed, bool notch)
{
    return inside && armed && !rejected && !lost && !menu_failed && notch;
}

static inline bool pingouin_fp_band_kept(PingouinFPRect frame, PingouinFPRect display, double notch)
{
    (void)frame;
    (void)display;
    (void)notch;
    return false;
}

static inline bool pingouin_fp_band_tall(PingouinFPRect frame, PingouinFPRect display, double notch)
{
    (void)frame;
    (void)display;
    (void)notch;
    return false;
}

typedef struct {
    bool inside, armed, entered, rejected, lost, display_changed, given_up;
    int tries;
    bool tried_this_settle, pending, notch, covers, band_kept, band_tall, display_live, same_size;
    bool on_space, visible, in_place, locked, moving, menu_bar_in_the_way, parts, reshape;
    bool appkit_band;
} PingouinFPRetakeFacts;

typedef enum {
    PINGOUIN_FP_RETAKE_NOW,
    PINGOUIN_FP_RETAKE_WAIT,
    PINGOUIN_FP_RETAKE_NO,
    PINGOUIN_FP_RETAKE_GIVE_UP,
} PingouinFPRetake;

/* 3.2.10 never laid a full screen out again. */
static inline PingouinFPRetake pingouin_fp_retake(PingouinFPRetakeFacts f, const char **why)
{
    (void)f;
    if (why) {
        *why = "3.2.10 has no retake";
    }
    return PINGOUIN_FP_RETAKE_NO;
}

/* 3.2.10 never asked again. */
static inline bool pingouin_fp_retake_reask(PingouinFPRetakeFacts f, PingouinFPRetake r, int reasks)
{
    (void)f;
    (void)r;
    (void)reasks;
    return false;
}

static inline bool pingouin_fp_retake_final(int tries)
{
    return tries >= PINGOUIN_FP_RETAKE_TRIES;
}

#endif
