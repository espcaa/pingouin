#!/bin/bash
# FullPanel's rules (pingouin-cocoa-fullpanel-logic.patch): the patch makes
# ui/pingouin-fullpanel.h in an empty folder (with ui/pingouin-clean-size.h for
# the guest's size), and test-fullpanel.c runs against it on every notched
# MacBook's size, external displays and a window moving between them. No
# QEMU, no display, no notch. CI and the runtime build run it.
# With ui/cocoa.m as argument (the runtime build, after the wiring patch):
# also that the wiring is there, every way in goes through the rules, and
# nothing of FullPanel's earlier implementation (a frame hook,
# frame guards, a forced full-screen size, hooks on AppKit's menu bar
# companion) is left (docs/adr/0044-fullpanel.md).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-fullpanel.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-fullpanel-logic.patch"
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-clean-size-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" \
  "$here/test-fullpanel.c" -o "$work/test-fullpanel"
"$work/test-fullpanel"

(( $# )) || exit 0
m=$1
fail() { echo "test-fullpanel: $m: $*" >&2; exit 1; }
body() { awk -v h="$1" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$m"; }
# Asked for per start only, through the header's rule.
[[ $(grep -c 'getenv("PINGOUIN_FULLPANEL")' "$m") == 1 ]] || fail "PINGOUIN_FULLPANEL is not read once"
grep -q 'return pingouin_fp_requested(getenv("PINGOUIN_FULLPANEL"));' "$m" || fail "the start's switch is not pingouin_fp_requested"
# Ready before the first full screen: the reservation answer and the way in.
grep -q '^    pingouin_fullpanel_init();$' "$m" || fail "FullPanel is not set up at the start"
awk '/^    pingouin_fullpanel_init\(\);$/ {i=NR} /\[controller doToggleFullScreen:nil\];/ {t=NR} END {exit !(i && t && i < t)}' "$m" ||
  fail "FullPanel is set up after the full-screen start"
init=$(body 'static void pingouin_fullpanel_init(void)')
grep -q 'addObserverForName:NSWindowWillEnterFullScreenNotification object:nil' <<<"$init" &&
  grep -q 'pingouin_fullpanel_prepare_window(w);' <<<"$init" || fail "a VM window's way into full screen is not prepared"
# The places that size the guest and the area ask the window first.
[[ $(grep -c 'pingouin_fullpanel_window_usable(' "$m") -ge 4 ]] || fail "screenSafeAreaSize, the guest's rows or the display box does not ask FullPanel"
grep -q 'if (below_notch && !pingouin_fullpanel_window_usable(\[cocoaView window\])) {' "$m" || fail "the display box ignores FullPanel"
# QEMU's own full-screen resize leaves a FullPanel window as AppKit laid it out
# (fitted to the firmware's 16:9 it left the strip: 1800x1013 on 1800x1169).
awk '/^- \(void\) (pingouinR|r)esizeWindow$/ {on=1} on {print} on && /setContentAspectRatio/ {exit}' "$m" |
  grep -q '    if (pingouin_fullpanel_window_usable(\[self window\])) {' ||
  fail "QEMU's full-screen resize fits a FullPanel window to the guest's mode"
# The guest's size: no rows cut over the whole panel (black band at the bottom).
grep -q 'full = pingouin_fp_cut_rows(isFullscreen, pingouin_present_layer(),' "$m" || fail "the guest's rows are cut in FullPanel (clean-size)"
# The layout is AppKit's: the reservation answer, scoped and checked.
hook=$(body 'static bool pingouin_fullpanel_install_reservation(void)')
grep -q '"_NSFullScreenContentController"' "$m" && grep -q '"reservesSpaceForMenuBarInFullScreen", "B16@0:8"' "$m" &&
  grep -q '"window", "@16@0:8"' "$m" || fail "the reservation method is not looked up by name and encoding"
grep -q 'BOOL keeps = appkit(controller, reserves_sel);' <<<"$hook" &&
  grep -q 'if (!keeps || !\[NSThread isMainThread\]) {' <<<"$hook" &&
  grep -q 'return state && \[state claimsStrip\] ? NO : keeps;' <<<"$hook" ||
  fail "the reservation answer is not AppKit's first, main thread only, for a claiming window only"
[[ $(grep -c 'method_setImplementation(' "$m") == 1 ]] || fail "more than the reservation method is replaced"
grep -q 'pingouin_fullpanel_in_image(now, cls)' "$m" || fail "another library's change of the method is not detected"
# The full screen counts once AppKit covered the display; final at didEnterFullScreen.
grep -q 'verdict = pingouin_fp_verdict(\[self claimsStrip\]' "$m" &&
  body '- (void)didEnterFullScreen:(NSNotification *)notification' | grep -q '    \[self verify:true\];' ||
  fail "the full screen is not verified (Split View)"
grep -q 'pingouin_fp_lost(accepted, true, covers,' "$m" || fail "a lost strip is not seen"
# A claim that ends (rejected, lost, a display without a housing) or comes back:
# QEMU's own handlers may have sized the guest before it, so they run again.
relayout=$(body '- (void)relayout')
grep -q 'windowDidResize:\[NSNotification' <<<"$relayout" && grep -q 'pingouin_displays_update();' <<<"$relayout" ||
  fail "the relayout does not run QEMU's resize handler and the arrangement"
body '- (void)verify:(bool)definitive' | grep -q '    \[self relayout\];' ||
  fail "a rejected full screen (Split View) keeps the guest's FullPanel size"
[[ $(grep -c '\[self relayout\];' "$m") == 4 ]] || fail "not every change of the claim lays out again"
# A new display mode is no lost strip: CoreGraphics has the mode before NSScreen
# (a 1512x982 mode looked like a lost strip on the old 1800x1169 frame).
grep -q 'pingouin_fullpanel_same_display(screen, accepted_display)' "$m" &&
  grep -q 'CGDisplayBounds(\[n unsignedIntValue\])' "$m" || fail "a display mode change counts as a lost strip"
grep -q 'pingouin_fp_screen_releases(armed && inside, full,' "$m" || fail "a display without a notch is not released"
# An external display's fallback beside the open MacBook is not the line check reads.
grep -q 'if (pingouin_fp_fallback_checked(fallback, pingouin_fullpanel_beside_builtin(window))) {' "$m" &&
  grep -q 'CGDisplayIsBuiltin(' <<<"$(body 'static bool pingouin_fullpanel_beside_builtin(NSWindow *w)')" ||
  fail "an external display's fallback can be the last line pingouin check reads"
# "Always show the menu bar in full screen" only without Keep the Dock away (#356).
grep -q 'pingouin_fp_menu_bar_in_the_way(always, immersive_mode_enabled)' "$m" || fail "the menu bar setting counts with Keep the Dock away"
# Keep the Dock away (immersive=on): the menu bar never comes down in FullPanel.
grep -q '    reveal = !immersive_mode_enabled;' "$m" || fail "the reveal does not follow Keep the Dock away"
# One place applies the intent; the reveal is seen through the titlebar window (KVO).
apply=$(body '- (bool)apply:(bool)submit')
grep -q 'PingouinFPIntent intent = pingouin_fp_intent(facts);' <<<"$apply" || fail "the intent is not pingouin_fp_intent's"
grep -q '@"NSToolbarFullScreenWindow"' "$m" &&
  grep -q 'addObserver:self forKeyPath:@"alphaValue"' "$m" || fail "the reveal is not seen through the titlebar window"
[[ $(grep -c 'QEMU_BUILD_BUG_ON(NSApplicationPresentation' "$m") == 5 ]] || fail "the presentation bits are not checked against AppKit's"
# The decisions are the tested ones.
for f in pingouin_fp_fallback pingouin_fp_fallback_checked pingouin_fp_hook pingouin_fp_verdict pingouin_fp_lost pingouin_fp_screen_releases pingouin_fp_screen_resumes pingouin_fp_cut_rows pingouin_fp_zone pingouin_fp_asked_after_move pingouin_fp_intent pingouin_fp_relax_target pingouin_fp_own_options pingouin_fp_restore pingouin_fp_compositor_matches pingouin_fp_return_ready pingouin_fp_return_again pingouin_fp_return_reassert pingouin_fp_menu_bar_in_the_way; do
  grep -q "$f(" "$m" || fail "$f is not used"
done
# Private interfaces only looked up at run time (no link to SkyLight).
grep -q 'dlopen(' "$m" && grep -q '"SLSTransactionSetMenuBarSystemOverrideAlpha"' "$m" || fail "SkyLight is not looked up at run time"
# SLSTransactionCommit returns nothing (macOS 27: w0 left over): never read as a status.
grep -q '^typedef void (\*PingouinSLSCommit)(CFTypeRef transaction, int32_t synchronous);$' "$m" ||
  fail "SLSTransactionCommit is not declared void"
! grep -q '= pingouin_sls_commit(' "$m" || fail "SLSTransactionCommit's leftover w0 is read as a status"
# Nothing of the earlier version is left.
for old in _frameForFullScreenMode _tileFrameForFullScreen _NSFullScreenMenuBarCompanionController \
           setMenuBarReveal: setToolbarWindowReveal: pingouin_fullpanel_keep_frame pingouin_fullpanel_frame_hook \
           pingouin_fp_frame pingouin_fp_keep pingouin_fp_strip_lost pingouin_fp_reveal_allowed pingouin_fp_presentation \
           updateMenuBarHidden showMenuBar updatePresentationOptions applyMenuBarReveal fullScreenToolbarWindow \
           otherWindowDidChangeOcclusionState fullScreenFrameForAppKitFrame menu_bar_hidden_space; do
  ! grep -q "$old" "$m" || fail "the earlier version's $old is still there"
done
! grep -q 'pingouin_fullpanel_prepare_window(self);' "$m" || fail "a window class still prepares FullPanel itself (-toggleFullScreen:)"
fs=$(body '- (NSSize)window:(NSWindow *)window willUseFullScreenContentSize:(NSSize)proposedSize')
! grep -q 'pingouin_fullpanel' <<<"$fs" || fail "the full-screen content size is still forced (AppKit proposes it)"
# What pingouin check reads.
grep -q '"pingouin: full panel: strip covered: ' "$m" && grep -q '"pingouin: full panel: strip lost: ' "$m" ||
  fail "no 'strip covered' / 'strip lost' line for pingouin check"
echo "test-fullpanel: $m: wiring ok"
