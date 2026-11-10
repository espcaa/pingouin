#!/bin/bash
# A window out of sight keeps the slow refresh tick
# (pingouin-cocoa-idle-refresh-unseen.patch), checked in the patched ui/cocoa.m:
#   - cocoa_gl_refresh tells the tick about a frame only when the window can
#     show it, and asks for no present while it cannot;
#   - "cannot show it" is present-unseen's test: the occlusion state and
#     PINGOUIN_PRESENT_HIDDEN, on the IOSurface present path only;
#   - the frame is still drawn when the window shows again (present_bh);
#   - with --self-test (CI): the checks fail on the code before the patch.
#   test-idle-refresh-unseen.sh <patched ui/cocoa.m> | --self-test
set -uo pipefail
body() { awk -v h="$2" 'index($0, h) == 1 {on=1} on {print} on && /^}$/ {exit}' "$1"; }
problems() {
  local m=$1 refresh held
  refresh=$(body "$m" 'static void cocoa_gl_refresh(DisplayChangeListener *dcl)')
  held=$(body "$m" 'static bool pingouin_present_held(void)')
  [[ -n $refresh ]] || { echo "no cocoa_gl_refresh"; return; }
  [[ -n $held ]] || { echo "no pingouin_present_held"; return; }
  grep -q 'return present_ctx && !qatomic_read(&pingouin_window_seen) && !always;' <<<"$held" &&
    grep -q 'getenv("PINGOUIN_PRESENT_HIDDEN")' <<<"$held" || echo "held is not present-unseen's out-of-sight test"
  grep -q 'bool held = pingouin_present_held();' <<<"$refresh" || echo "the refresh does not ask whether the frame is held"
  grep -q 'cocoa_refresh_tick(gl_dirty && !held);' <<<"$refresh" || echo "a held frame still keeps the tick fast"
  grep -q 'if (gl_dirty && !held) {' <<<"$refresh" || echo "every tick still asks to present a held frame"
  grep -q 'cocoa_refresh_tick(gl_dirty);' <<<"$refresh" && echo "the old tick call is still there"
  grep -q 'qemu_bh_schedule(present_bh);' "$m" || echo "the frame is not drawn when the window shows again"
}
if [[ ${1:-} == --self-test ]]; then
  T=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-refresh-unseen.XXXXXX")
  trap 'rm -rf "$T"' EXIT
  printf 'static void cocoa_gl_refresh(DisplayChangeListener *dcl)\n{\n    cocoa_refresh(dcl);\n    cocoa_refresh_tick(gl_dirty);\n\n    if (gl_dirty) {\n    }\n}\n' > "$T/old.m"
  if [[ -n $(problems "$T/old.m") ]]; then echo "ok   the checks fail on the code before the patch"; exit 0; fi
  echo "FAIL the checks pass on the code before the patch" >&2; exit 1
fi
m=${1:?usage: test-idle-refresh-unseen.sh <patched ui/cocoa.m> | --self-test}
out=$(problems "$m")
if [[ -n $out ]]; then echo "test-idle-refresh-unseen: $m: $out" >&2; exit 1; fi
echo "test-idle-refresh-unseen: $m: wiring ok"
