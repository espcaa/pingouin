#!/bin/bash
# The guest's modifier keys follow each event's flags (#412,
# pingouin-cocoa-modifier-state-logic.patch, pingouin-cocoa-modifier-state.patch):
# the patch makes ui/pingouin-modifiers.h in an empty folder and
# test-modifier-state.c runs the report's sequences, every pair and side and a
# random run against it, and QEMU's old rules, which must fail them. With
# ui/cocoa.m as argument (the runtime build, after the wiring patch): also that
# every input event goes through it and nothing flips a key any more.
#   test-modifier-state.sh                       (CI)
#   test-modifier-state.sh <patched ui/cocoa.m>  (the runtime build)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-modifier-state.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-modifier-state-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" "$here/test-modifier-state.c" -o "$work/t"
"$work/t"

(( $# )) || exit 0
m=$1
fail() { echo "test-modifier-state: $m: $*" >&2; exit 1; }
body() { awk -v h="$1" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$m"; }
sync=$(body '- (void) pingouinSyncModifiers:(NSEvent *)event')
handle=$(body '- (bool) handleEventLocked:(NSEvent *)event')
[[ -n $sync && -n $handle ]] || fail "pingouinSyncModifiers or handleEventLocked is missing"
grep -q '#include "ui/pingouin-modifiers.h"' "$m" || fail "ui/pingouin-modifiers.h is not used"
# Nothing flips a key; no pair is released by hand outside the rules.
! grep -q 'toggleKey' "$m" || fail "a key is still flipped (toggleKey)"
! grep -qE 'if \(!\(modifiers & NSEventModifierFlag(Shift|Control|Option|Command)\)\)' <<<"$handle" ||
  fail "handleEventLocked still releases a pair by its flag"
# Every input event: after the Caps Lock check, before the injected text and the event's own handling.
awk '/qkbd_state_key_event\(kbd, KEY_CAPSLOCK, false\);/{c=NR} /\[self pingouinSyncModifiers:event\];/{s=NR; n++}
     /\[self handleInjectedText:event\]/{t=NR} /switch \(\[event type\]\) \{/{if (s && !w) w=NR}
     END{exit !(n == 1 && c && s > c && t > s && w > t)}' <<<"$handle" ||
  fail "the modifiers are not set once per event before it is handled"
# A key going down or a modifier changing may press; Command only while the VM has the keyboard.
grep -q 'PingouinModEvent kind = type == NSEventTypeKeyDown || type == NSEventTypeFlagsChanged ?' <<<"$sync" &&
  grep -q 'PINGOUIN_MOD_EV_KEY : PINGOUIN_MOD_EV_INPUT;' <<<"$sync" || fail "the event kinds are not as tested"
grep -q 'int code = type == NSEventTypeFlagsChanged ? \[event keyCode\] : -1;' <<<"$sync" ||
  fail "a flagsChanged's key code is not passed"
grep -q 'next\[p\] = pingouin_mod_next(p, now\[p\], kind, \[event modifierFlags\], code,' <<<"$sync" &&
  grep -q '!command || (captured && left_command_key_enabled),' <<<"$sync" &&
  grep -q '!command || captured);' <<<"$sync" && grep -q 'bool captured = \[self isKeyboardCaptured\];' <<<"$sync" ||
  fail "Command does not follow the keyboard capture (and left-command-key)"
# Option and Command swap with swap-opt-cmd, as before.
grep -q 'if (swap_opt_cmd) {' <<<"$sync" && grep -q 'keys\[PINGOUIN_MOD_OPTION\]\[0\] = KEY_LEFTMETA;' <<<"$sync" &&
  grep -q 'keys\[PINGOUIN_MOD_COMMAND\]\[0\] = KEY_LEFTALT;' <<<"$sync" || fail "swap-opt-cmd is not kept"
# Releases before presses; a key only sent when it changes.
awk '/qkbd_state_key_event\(kbd, keys\[p\]\[0\], false\);/{r=NR} /qkbd_state_key_event\(kbd, keys\[p\]\[0\], true\);/{p=NR}
     END{exit !(r && p && r < p)}' <<<"$sync" || fail "presses come before releases"
grep -q 'if (now\[p\].left && !next\[p\].left) {' <<<"$sync" && grep -q 'if (!now\[p\].left && next\[p\].left) {' <<<"$sync" ||
  fail "a key is sent without a change"
# The flagsChanged event itself only ends there.
awk '/pingouinSyncModifiers:event/{s=1} s && /case NSEventTypeFlagsChanged:/{f=NR} f && NR > f && !/^ *(\/\*.*\*\/)?$/ {print; exit}' <<<"$handle" | grep -q 'return true;' ||
  fail "flagsChanged does more than the flags"
echo "test-modifier-state: $m: wiring ok"
