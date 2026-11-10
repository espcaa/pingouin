#!/bin/bash
# Every scale in FullPanel (#355; pingouin-cocoa-fullpanel-scale-logic.patch,
# pingouin-cocoa-fullpanel-scale.patch): the patch makes
# ui/pingouin-fullpanel-scale.h in an empty folder and test-fullpanel-scale.c
# runs against it on every notched MacBook's panel and every one of Omarchy's
# presets. With ui/cocoa.m as argument (the runtime build): also the wiring.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-fullpanel-scale.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-fullpanel-scale-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" "$here/test-fullpanel-scale.c" -o "$work/t" -lm
"$work/t"

(( $# )) || exit 0
m=$1
fail() { echo "test-fullpanel-scale: $m: $*" >&2; exit 1; }
body() { awk -v h="$1" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$m"; }
ui=$(body '- (void) updateUIInfoLocked')
from=$(body 'static void pingouin_fp_scale_from_guest(double s)')
load=$(body 'static void pingouin_fp_scale_load(void)')
[[ -n $ui && -n $from && -n $load ]] || fail "a piece of the wiring is missing"
# FullPanel only (in full screen, the window usable), through the tested rule.
grep -q '        panel = isFullscreen && pingouin_fullpanel_window_usable(\[self window\]);' <<<"$ui" &&
  grep -q 'pingouin_fp_scale_size(clean.w, clean.h, qatomic_read(&pingouin_fp_scale_want),' <<<"$ui" ||
  fail "the guest's size in FullPanel does not follow its scale"
grep -q 'pingouin: full panel: guest %ux%u on the %ux%u panel' <<<"$ui" || fail "qemu.log does not say the guest's size"
# The guest's word: checked, kept in the VM's folder, applied at once in FullPanel.
grep -q 'if (pingouin_json_number(d\[@"scale"\], &scale)) {' "$m" || fail "the display port's scale is not read"
grep -q 'uint32_t k = pingouin_fp_scale_k(s);' <<<"$from" && grep -q 'getenv("PINGOUIN_FULLPANEL_SCALE_FILE")' <<<"$from" &&
  grep -q 'rename(tmp, file);' <<<"$from" && grep -q '\[cocoaView updateUIInfo\];' <<<"$from" ||
  fail "the guest's scale is not checked, kept and applied"
grep -q 'k >= PINGOUIN_FP_SCALE_MIN && k <= PINGOUIN_FP_SCALE_MAX' <<<"$load" || fail "the kept scale is not checked at start"
# The picture fills the panel: the layer's default gravity is resize (no band in FullPanel).
grep -q 'NSString \*g = kCAGravityResize;' "$m" || fail "the picture is not scaled onto the view"
# The pointer: x by the width (the guest a few pixels wider than the panel).
grep -q 'qemu_input_queue_abs(dcl.con, INPUT_AXIS_X, p.x \* dx, 0, screen.width);' "$m" || fail "the pointer's x uses the height's ratio"
echo "test-fullpanel-scale: $m: wiring ok"
