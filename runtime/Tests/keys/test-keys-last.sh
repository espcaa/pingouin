#!/bin/bash
# QEMU's full-grab tap and keyboard sharing apps (#350,
# pingouin-cocoa-keys-last.patch): with PINGOUIN_KEYS_LAST=1 (Pingouin.app's hidden
# shareKeyboard) the tap goes at the end of macOS's event tap chain, so a
# keyboard sharing app (Synergy, Deskflow, ...) sees the keys first; without
# it at the head, as before. pingouin_keys_place() runs in test-keys-last.c.
#   test-keys-last.sh                       (CI: the function from the patch)
#   test-keys-last.sh <patched ui/cocoa.m>  (the runtime build: also the wiring)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-keys-last.XXXXXX")
trap 'rm -rf "$work"' EXIT
fn='static CGEventTapPlacement pingouin_keys_place(void)'
if [[ $# -ge 1 ]]; then
  m=$1
  awk -v h="$fn" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$m" > "$work/keys-place.inc"
else
  awk -v h="+$fn" '$0 == h {on=1} on {print substr($0, 2)} on && /^\+}$/ {exit}' \
    "$patches/pingouin-cocoa-keys-last.patch" > "$work/keys-place.inc"
fi
[[ -s $work/keys-place.inc ]] || { echo "FAIL pingouin_keys_place not found"; exit 1; }
cc -Wall -Wextra -Werror -I"$work" "$here/test-keys-last.c" -framework ApplicationServices -o "$work/t"
"$work/t"
(( $# )) || exit 0
fail() { echo "test-keys-last: $m: $*" >&2; exit 1; }
# One tap, made at that place; the place said in qemu.log.
[[ $(grep -c 'CGEventTapCreate(' "$m") == 1 ]] || fail "not exactly one CGEventTapCreate"
grep -q '^    CGEventTapPlacement place = pingouin_keys_place();$' "$m" &&
  grep -q '^    eventsTap = CGEventTapCreate(kCGHIDEventTap, place, kCGEventTapOptionDefault,$' "$m" ||
  fail "setFullGrab's tap is not made at pingouin_keys_place()"
grep -q 'full-grab tap at the end of the event tap chain' "$m" || fail "qemu.log does not say the tap is at the end"
echo "test-keys-last: $m: wiring ok"
