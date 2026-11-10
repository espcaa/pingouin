#!/bin/bash
# QEMU's full-grab tap without its permission (pingouin-cocoa-tap-permission.patch,
# issue #192): reenableEventTap, pingouinTapAllowed, pingouinKeepTap and
# pingouinAskPostEvent run in a small program with a made-up tap (a plain Mach
# port on the run loop) and made-up answers for Accessibility and "control the
# computer". One the tap was made with taken away: never enabled again, and
# the tap, its port and run-loop source are gone right after the callback;
# still there: enabled again and kept; an app allowed only through "control
# the computer" keeps its tap. No QEMU, no permissions.
#   test-tap-permission.sh                       (CI: the methods from the patch)
#   test-tap-permission.sh <patched ui/cocoa.m>  (the runtime build)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-tap-permission.XXXXXX")
trap 'rm -rf "$work"' EXIT
if [[ $# -ge 1 ]]; then
  src=$1
else
  # The patch's new side: context and added lines, without the removed ones.
  src=$work/new.m
  awk '/^@@/ {on=1; next} on && /^-/ {next} on {print substr($0, 2)}' \
    "$patches/pingouin-cocoa-tap-permission.patch" > "$src"
fi
body() { awk -v h="$2" '$0 == h {on=1} on {print} on && /^}$/ {exit}' "$1"; }
methods=
for m in '- (void) reenableEventTap' '- (BOOL) pingouinTapAllowed' '- (BOOL) pingouinKeepTap' '- (void) pingouinAskPostEvent'; do
  b=$(body "$src" "$m")
  [[ -n $b ]] || { echo "FAIL $m not found in $src"; exit 1; }
  methods+=$b$'\n'
done
# What setFullGrab adds once its tap is made (a method of its own here).
watch=$(awk '/\/\* Pingouin: the tap goes when a permission it was made with does/ {on=1} on {print} on && /^}$/ {exit}' "$src")
[[ -n $watch ]] || { echo "FAIL setFullGrab's permission watch not found in $src"; exit 1; }
fail=0
need() { if grep -qF "$2" "$src"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
need "setFullGrab notes what the tap was made with" 'pingouinTapMadeWithPost = pingouinTapPost;'
need "... and Accessibility" 'pingouinTapMadeWithAX = AXIsProcessTrusted();'
need "... looks every second (common modes)" 'NSTimer *permission = [NSTimer timerWithTimeInterval:1.0 repeats:YES'
need "... with the run loop's common modes" '[[NSRunLoop mainRunLoop] addTimer:permission forMode:NSRunLoopCommonModes];'
need "... and on macOS's Accessibility list changing" 'addObserverForName:@"com.apple.accessibility.api"'
cat > "$work/test.m" <<OBJC
#import <Foundation/Foundation.h>
#import <ApplicationServices/ApplicationServices.h>
static int trusted = 1, post = 1, enabled, warned, asked;
static Boolean fakeTrusted(void) { return trusted; }
static bool fakePost(void) { asked++; return post; }
static void fakeEnable(CFMachPortRef t, bool on) { (void)t; enabled = on; }
#define AXIsProcessTrusted fakeTrusted
#define CGPreflightPostEventAccess fakePost
#define CGEventTapEnable fakeEnable
#define warn_report(...) (warned++)
@interface View : NSObject {
@public
  CFMachPortRef eventsTap;
  BOOL pingouinTapMadeWithAX, pingouinTapMadeWithPost, pingouinTapPost, pingouinAskingPost;
}
- (void) reenableEventTap;
- (BOOL) pingouinTapAllowed;
- (BOOL) pingouinKeepTap;
- (void) pingouinAskPostEvent;
- (void) watchTap;
@end
@implementation View
$methods
- (void) watchTap
{
$watch
@end
static int failed;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failed = 1; }
static void drain(void) { for (int i = 0; i < 20; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false); }
// A view whose tap is a plain Mach port on the main run loop, made with these permissions.
static View *made(BOOL ax, BOOL withPost, CFRunLoopSourceRef *source) {
  View *v = [View new];
  CFMachPortRef port = CFMachPortCreate(NULL, NULL, NULL, NULL);
  CFRunLoopSourceRef s = CFMachPortCreateRunLoopSource(NULL, port, 0);
  CFRunLoopAddSource(CFRunLoopGetMain(), s, kCFRunLoopDefaultMode);
  v->eventsTap = port;
  v->pingouinTapMadeWithAX = ax; v->pingouinTapMadeWithPost = withPost; v->pingouinTapPost = withPost;
  if (source) *source = s; else CFRelease(s);
  return v;
}
static int gone(View *v, CFMachPortRef port, CFRunLoopSourceRef s) {
  return !v->eventsTap && !CFMachPortIsValid(port) && (!s || (!CFRunLoopSourceIsValid(s) &&
         !CFRunLoopContainsSource(CFRunLoopGetMain(), s, kCFRunLoopDefaultMode)));
}
int main(void) {
  // Made with Accessibility and "control the computer" (both granted).
  CFRunLoopSourceRef src;
  View *v = made(YES, YES, &src);
  CFMachPortRef port = v->eventsTap; CFRetain(port);
  [v reenableEventTap];
  check(enabled && v->eventsTap == port, "disabled by macOS, both there: enabled again");
  drain();
  check(asked == 1 && v->eventsTap == port, "... and 'control the computer' asked again (off the main thread)");
  check([v pingouinKeepTap] && v->eventsTap == port && CFMachPortIsValid(port), "the check keeps it");
  enabled = 0; trusted = 0;
  [v reenableEventTap];
  check(!enabled && v->eventsTap == port, "disabled by macOS, Accessibility gone: not enabled again (still there in the callback)");
  drain();
  check(gone(v, port, src) && warned == 1, "... the tap, its port and run-loop source gone right after the callback, said once");
  trusted = 1;
  [v reenableEventTap];
  check(!enabled && ![v pingouinKeepTap], "no tap any more: nothing enabled, the check says so (its timer stops)");
  CFRelease(port); CFRelease(src);

  // The every-second check (or the Accessibility list changed) removes it at once.
  View *w = made(YES, YES, NULL);
  CFMachPortRef p2 = w->eventsTap; CFRetain(p2);
  trusted = 0;
  check(![w pingouinKeepTap] && gone(w, p2, NULL) && warned == 2, "the check without Accessibility removes it at once");
  trusted = 1; CFRelease(p2);

  // Allowed only through "control the computer" (CGRequestPostEventAccess):
  // Accessibility alone does not decide.
  View *x = made(NO, YES, &src);
  CFMachPortRef p3 = x->eventsTap; CFRetain(p3);
  trusted = 0; enabled = 0;
  [x reenableEventTap];
  drain();
  check(enabled && x->eventsTap == p3 && [x pingouinKeepTap], "made without Accessibility, 'control the computer' there: kept and enabled again");
  post = 0; enabled = 0;
  [x pingouinAskPostEvent];
  check(x->eventsTap == p3, "... the answer comes back on the main thread, not in the call");
  drain();
  check(gone(x, p3, src) && warned == 3, "'control the computer' taken away: the next answer (every 2 s) removes it");
  CFRelease(p3); CFRelease(src);

  // Disabled by macOS while the last answer still said yes: enabled, asked
  // again at once, and removed by that answer.
  View *y = made(NO, YES, &src);
  CFMachPortRef p4 = y->eventsTap; CFRetain(p4);
  post = 0; enabled = 0;
  [y reenableEventTap];
  drain();
  check(gone(y, p4, src) && warned == 4, "disabled by macOS with an old answer: asked again and removed right after");
  CFRelease(p4); CFRelease(src);

  // setFullGrab's watch: what the tap was made with, then the every-second
  // timer (it asks "control the computer" every 2 s).
  post = 1; trusted = 0;
  View *z = made(NO, NO, &src);
  CFMachPortRef p5 = z->eventsTap; CFRetain(p5);
  [z watchTap];
  check(!z->pingouinTapMadeWithAX && z->pingouinTapMadeWithPost && z->pingouinTapPost,
        "setFullGrab notes the tap was made with 'control the computer', without Accessibility");
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.2, false);
  check(z->eventsTap == p5, "... and its timer keeps it while that stays");
  post = 0;
  for (int i = 0; i < 30 && z->eventsTap; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
  check(gone(z, p5, src) && warned == 5, "... and removes it within 2 s once it is taken away");
  CFRelease(p5); CFRelease(src);
  return failed;
}
OBJC
cc -fblocks -Wall -Wextra -Werror -Wno-unused-parameter "$work/test.m" -framework Foundation -framework ApplicationServices \
  -o "$work/test" || { echo "FAIL the methods do not compile"; exit 1; }
"$work/test" || fail=1
exit $fail
