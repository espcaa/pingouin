#!/bin/bash
# No macOS prompt for QEMU's full-grab tap at the VM's start
# (pingouin-cocoa-tap-no-ask.patch): CGEventTapCreate for an app that may not
# control the computer made macOS show "... would like to control this Mac" at
# every VM start. pingouin_tap_allowed() runs in test-tap-no-ask.c with made-up
# answers; the runtime build also checks that setFullGrab asks it before its
# only CGEventTapCreate and stops there with qemu.log's usual line.
#   test-tap-no-ask.sh                       (CI: the function from the patch)
#   test-tap-no-ask.sh <patched ui/cocoa.m>  (the runtime build: also the wiring)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-tap-no-ask.XXXXXX")
trap 'rm -rf "$work"' EXIT
fn='static bool pingouin_tap_allowed(void)'
if [[ $# -ge 1 ]]; then
  m=$1
  awk -v h="$fn" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$m" > "$work/tap-allowed.inc"
else
  [[ -f $patches/pingouin-cocoa-tap-no-ask.patch ]] || { echo "FAIL pingouin-cocoa-tap-no-ask.patch is missing"; exit 1; }
  awk -v h="+$fn" '$0 == h {on=1} on {print substr($0, 2)} on && /^\+}$/ {exit}' \
    "$patches/pingouin-cocoa-tap-no-ask.patch" > "$work/tap-allowed.inc"
fi
[[ -s $work/tap-allowed.inc ]] || { echo "FAIL pingouin_tap_allowed not found"; exit 1; }
cc -Wall -Wextra -Werror -I"$work" "$here/test-tap-no-ask.c" -o "$work/t"
"$work/t"
(( $# )) || exit 0
fail() { echo "test-tap-no-ask: $m: $*" >&2; exit 1; }
[[ $(grep -c 'CGEventTapCreate(' "$m") == 1 ]] || fail "not exactly one CGEventTapCreate"
# setFullGrab: the answer first, then (only when allowed) the tap.
awk '
  /^- \(void\) setFullGrab:\(id\)sender$/ {on = 1}
  on && /^    if \(!pingouin_tap_allowed\(\)\) \{$/ {guard = NR}
  on && guard && NR == guard + 1 && /warn_report\("Could not create event tap/ {said = 1}
  on && guard && NR == guard + 2 && /^        return;$/ {ret = 1}
  on && /CGEventTapCreate\(/ {tap = NR}
  on && /^}$/ {exit}
  END {exit !(guard && said && ret && tap > guard)}
' "$m" || fail "setFullGrab does not ask pingouin_tap_allowed() before CGEventTapCreate (and stop with qemu.log's line)"
echo "test-tap-no-ask: $m: wiring ok"
