#!/bin/bash
# Closing the window (#358; pingouin-cocoa-close-logic.patch,
# pingouin-cocoa-close.patch): the patch makes ui/pingouin-close.h in an empty
# folder and test-close.c runs against it. With ui/cocoa.m as argument (the
# runtime build, after the wiring patch): also that the close button and the
# Dock icon use it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-close.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-close-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" "$here/test-close.c" -o "$work/test-close"
"$work/test-close"

(( $# )) || exit 0
m=$1
fail() { echo "test-close: $m: $*" >&2; exit 1; }
body() { awk -v h="$1" 'index($0, h) == 1 {on=1} on {print} on && /^}$/ {exit}' "$m"; }
close=$(body "/* Called when the user clicks on a window's close button */")
now=$(body 'static PingouinClose pingouin_close_now(void)')
reopen=$(body '- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag')
ask=$(body '- (void)pingouinAskClose')
keep=$(body '- (void)pingouinKeepRunning')
[[ -n $close && -n $now && -n $reopen && -n $ask && -n $keep ]] || fail "a piece of the wiring is missing"
# The close button: the setting first; Shut down (and anything else) quits as before.
grep -q 'switch (pingouin_close_now()) {' <<<"$close" && grep -q '\[self pingouinKeepRunning\];' <<<"$close" &&
  grep -q '\[self pingouinAskClose\];' <<<"$close" && grep -q '\[NSApp terminate: *sender\]' <<<"$close" ||
  fail "the close button does not follow the setting"
# The setting: the app's own (live), else the start's.
grep -q 'objectForKey:@"closeAction"' <<<"$now" && grep -q 'getenv("PINGOUIN_CLOSE_ACTION")' <<<"$now" &&
  grep -q 'return pingouin_close_pick(app, ' <<<"$now" || fail "the setting is not read as tested"
# Keep running: hidden, never terminated; logged.
grep -q '\[NSApp hide:self\];' <<<"$keep" && ! grep -q 'terminate' <<<"$keep" &&
  grep -q 'pingouin: close: the window is hidden, the VM keeps running' <<<"$keep" || fail "Keep running does not hide the app"
# Ask: three buttons, Shut Down first (Return), Cancel last (Esc).
awk '/addButtonWithTitle:@"Shut Down"/{a=NR} /addButtonWithTitle:@"Keep Running"/{b=NR} /addButtonWithTitle:@"Cancel"/{c=NR}
     END{exit !(a && b && c && a < b && b < c)}' <<<"$ask" || fail "the question's buttons are not Shut Down, Keep Running, Cancel"
# The question names the VM (-name), not the window's title ("... (Press ⌃⌥G to release Mouse)").
grep -q 'stringWithUTF8String:qemu_name' <<<"$ask" && ! grep -q '\[w title\]' <<<"$ask" || fail "the question does not name the VM"
grep -q 'NSAlertFirstButtonReturn' <<<"$ask" && grep -q '\[NSApp terminate:self\];' <<<"$ask" &&
  grep -q 'NSAlertSecondButtonReturn' <<<"$ask" || fail "the answers are not acted on"
# The Dock icon brings it back (hidden or minimised).
grep -q '\[NSApp unhide:self\];' <<<"$reopen" && grep -q 'deminiaturize' <<<"$reopen" || fail "the Dock icon does not show the window again"
echo "test-close: $m: wiring ok"
