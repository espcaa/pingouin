// Based on omacvm's macOS gestures helper (MIT, Gilles Goetsch).
//
// Mac side of the VM's trackpad gestures and Cmd shortcuts, linked into
// Pingouin.app. While the target process (QEMU) is the frontmost app and has
// a full-screen window ("capture"):
//   * macOS's trackpad gesture events (Spaces / Mission Control swipes, pinch,
//     rotate, smart zoom) are dropped by an event tap, so macOS reacts to none
//     of them;
//   * raw finger contacts from the trackpads (MultitouchSupport, a private
//     framework, loaded at run time) go out as "F" lines: every frame with 3+
//     fingers, and 2-finger frames once they are a pinch. One-finger movement,
//     clicks and two-finger scrolling stay on QEMU's own path;
//   * Cmd+key goes out as "K" lines (Super+key on the guest's virtual
//     keyboard) instead of reaching macOS.
// Ctrl+Option+Esc releases capture ("S esc"), handing everything back to
// macOS; pressed again while the VM is still in front, capture comes back.
// With Three Finger Drag on in macOS, three fingers stay macOS's drag.
// If Accessibility or Input Monitoring is taken away while running, the tap
// and trackpads are let go at once and come back when the permission does.
//
// Everything runs on one internal thread with its own run loop (the event tap,
// the capture check, timers); MultitouchSupport calls back on its own thread.
// Lines go out through the pg_send_fn callback, serialized by a mutex.
#include "pingouin_gestures.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // GetFrontProcess, KBGetLayoutType

// ---- MultitouchSupport (private framework, resolved with dlopen) ----
typedef struct { float x, y; } MTPoint;
typedef struct { MTPoint pos, vel; } MTVector;
typedef struct {
  int32_t frame; double timestamp; int32_t pathIndex, state, fingerID, handID;
  MTVector normalized; float zTotal; int32_t f9; float angle, majorAxis, minorAxis;
  MTVector absolute; int32_t f14, f15; float zDensity;
} MTTouch;
typedef void *MTDeviceRef;
typedef int (*MTFrameCallback)(MTDeviceRef, MTTouch *, int, double, int);
static struct {
  CFArrayRef (*createList)(void);
  void (*registerCb)(MTDeviceRef, MTFrameCallback);
  void (*unregisterCb)(MTDeviceRef, MTFrameCallback);
  int (*start)(MTDeviceRef, int);   // an IOReturn, 0: started
  void (*stop)(MTDeviceRef);
  bool (*isBuiltIn)(MTDeviceRef);
  int (*dimensions)(MTDeviceRef, int *, int *);   // 1/100 mm
  int (*deviceID)(MTDeviceRef, uint64_t *);
  bool (*isRunning)(MTDeviceRef);
  io_service_t (*service)(MTDeviceRef);
} mt;

static int loadMultitouch(void) {
  static int loaded = -1;
  if (loaded >= 0) return loaded;
  void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_LAZY);
  if (!h) return loaded = 0;
#define SYM(field, name) (*(void **)&mt.field = dlsym(h, name))
  int ok = SYM(createList, "MTDeviceCreateList") && SYM(registerCb, "MTRegisterContactFrameCallback") &&
           SYM(unregisterCb, "MTUnregisterContactFrameCallback") && SYM(start, "MTDeviceStart") &&
           SYM(stop, "MTDeviceStop") && SYM(isBuiltIn, "MTDeviceIsBuiltIn") &&
           SYM(dimensions, "MTDeviceGetSensorSurfaceDimensions") && SYM(deviceID, "MTDeviceGetDeviceID") &&
           SYM(isRunning, "MTDeviceIsRunning") && SYM(service, "MTDeviceGetService");
#undef SYM
  return loaded = ok ? 1 : 0;
}

// ---- state ----
#define ESC_KEYCODE 53
#define PINCH_SPREAD 0.035f   // normalized change of finger distance that makes a pinch
#define PINCH_RATIO 1.3f      // ... and it must exceed the centroid movement by this much
#define PERM_AX PG_PERM_ACCESSIBILITY
#define PERM_IM PG_PERM_INPUT_MONITORING
#define PERM_POST 4           // "control the computer" (CGPreflightPostEventAccess)

// Recursive: a send callback may call pg_hello.
static pthread_mutex_t sendLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pg_send_fn sendFn;      // under sendLock
static void *sendCtx;
static pid_t targetPid;

static pthread_t worker;
static CFRunLoopRef loop;      // the worker's run loop (retained; set and cleared under loopLock)
static pthread_mutex_t loopLock = PTHREAD_MUTEX_INITIALIZER;
static volatile int quit;
static int started;            // pg_start/pg_stop (caller's thread)

static volatile int frontIsTarget, escaped, capturing;
static volatile int threeDrag;   // macOS's Three Finger Drag is on
static volatile int fingers;     // contacts in the touching trackpad's last frame

static CFMachPortRef tapPort;
static CFRunLoopSourceRef tapSource;
static CGEventMask tapMask;

static void pg_log(const char *fmt, ...) {
  time_t t = time(NULL); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "%s pingouin-gestures: ", ts); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap);
}

static double monoNow(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

// A block on the worker's run loop, now or after `secs`.
static void onLoop(void (^f)(void)) {
  pthread_mutex_lock(&loopLock);
  if (loop && !quit) { CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, f); CFRunLoopWakeUp(loop); }
  pthread_mutex_unlock(&loopLock);
}
// Worker thread only.
static void afterOnLoop(double secs, void (^f)(void)) {
  if (!loop || quit) return;
  CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + secs, 0, 0, 0,
                                                        ^(CFRunLoopTimerRef x) { (void)x; if (!quit) f(); });
  CFRunLoopAddTimer(loop, t, kCFRunLoopCommonModes);
  CFRelease(t);
}

// ---- output ----
static void sendLine(const char *line, size_t len) {
  pthread_mutex_lock(&sendLock);
  if (sendFn) sendFn(line, len, sendCtx);
  pthread_mutex_unlock(&sendLock);
}

// The trackpads (built-in, Magic Trackpads, also ones connected later): the
// one touching now has its frames sent. entry: the IORegistry entry of its
// device (macOS makes a new one for the same trackpad at a wake from deep sleep).
#define MAX_PADS 8
static struct { MTDeviceRef dev; uint64_t id, entry; int w, h, fingers; } pads[MAX_PADS];
static int nPads, activePad = -1;
static int tpW = 15600, tpH = 9600;   // the touching trackpad, 1/100 mm (under padLock)
static pthread_mutex_t padLock = PTHREAD_MUTEX_INITIALIZER;

// "O <natural> <w> <h>": macOS's scrolling direction and the touching
// trackpad's size, so the guest scales finger movement for it.
static void sendSize(void) {
  CFPropertyListRef nat = CFPreferencesCopyAppValue(CFSTR("com.apple.swipescrolldirection"), kCFPreferencesAnyApplication);
  int natural = nat && CFGetTypeID(nat) == CFBooleanGetTypeID() ? CFBooleanGetValue((CFBooleanRef)nat) : 1;
  if (nat) CFRelease(nat);
  pthread_mutex_lock(&padLock);
  int w = tpW, h = tpH;
  pthread_mutex_unlock(&padLock);
  char b[64]; int n = snprintf(b, sizeof b, "O %d %d %d\n", natural, w, h);
  sendLine(b, (size_t)n);
}

static void sendState(const char *s) {
  char b[16]; int n = snprintf(b, sizeof b, "S %s\n", s);
  sendLine(b, (size_t)n);
}

// ---- touch forwarding (MultitouchSupport's thread) ----
static int forwarding;          // the last frame sent had fingers
static int pinchArmed, pinch;   // 2-finger gesture tracking
static float d0, cx0, cy0;

static int touching(const MTTouch *t) { return t->state >= 1 && t->state <= 5 && t->zTotal > 0.0f; }

static int frameCb(MTDeviceRef dev, MTTouch *touches, int n, double ts, int frame) {
  (void)ts; (void)frame;
  MTTouch *c[16]; int k = 0;
  for (int i = 0; i < n && k < 16; i++) if (touching(&touches[i])) c[k++] = &touches[i];

  // One trackpad at a time: another one's frames count once this one is free.
  pthread_mutex_lock(&padLock);
  int pad = -1;
  for (int i = 0; i < nPads; i++) if (pads[i].dev == dev) pad = i;
  if (pad < 0) { pthread_mutex_unlock(&padLock); return 0; }   // one let go (stopTrackpads)
  pads[pad].fingers = k;
  int switched = 0;
  if (pad != activePad) {
    if (k == 0 || (activePad >= 0 && pads[activePad].fingers > 0)) { pthread_mutex_unlock(&padLock); return 0; }
    activePad = pad;
    switched = pads[pad].w != tpW || pads[pad].h != tpH;
    if (switched) { tpW = pads[pad].w; tpH = pads[pad].h; }
  }
  pthread_mutex_unlock(&padLock);
  if (switched) sendSize();

  int send = 0;
  fingers = k;
  if (capturing) {
    // Three Finger Drag on: three fingers are macOS's drag (it reaches QEMU as
    // the pointer's), only four or more are the guest's.
    if (k >= 3 && !(k == 3 && threeDrag)) send = 1;
    else if (k == 2) {
      float dx = c[0]->normalized.pos.x - c[1]->normalized.pos.x, dy = c[0]->normalized.pos.y - c[1]->normalized.pos.y;
      float d = sqrtf(dx * dx + dy * dy);
      float cx = (c[0]->normalized.pos.x + c[1]->normalized.pos.x) / 2, cy = (c[0]->normalized.pos.y + c[1]->normalized.pos.y) / 2;
      if (!pinchArmed) { pinchArmed = 1; pinch = 0; d0 = d; cx0 = cx; cy0 = cy; }
      if (!pinch) {
        float spread = fabsf(d - d0), move = hypotf(cx - cx0, cy - cy0);
        if (spread > PINCH_SPREAD && spread > move * PINCH_RATIO) pinch = 1;
      }
      send = pinch;
    }
  }
  if (k != 2) pinchArmed = 0;

  if (send) {
    char buf[1024]; int len = snprintf(buf, sizeof buf, "F %d", k);
    for (int i = 0; i < k && len < (int)sizeof buf - 64; i++)
      len += snprintf(buf + len, sizeof buf - (size_t)len, " %d %.5f %.5f %.3f", c[i]->pathIndex,
                      c[i]->normalized.pos.x, 1.0f - c[i]->normalized.pos.y, c[i]->zTotal);
    buf[len++] = '\n';
    sendLine(buf, (size_t)len);
    forwarding = 1;
  } else if (forwarding) {
    sendLine("F 0\n", 4);   // gesture over (fingers lifted below the threshold, or capture ended)
    forwarding = 0;
  }
  return 0;
}

// ---- the trackpads ----
// MultitouchSupport lists every multi-touch surface (a Magic Mouse's too); a
// trackpad is the built-in one or one at least 100 mm wide. Looked for again
// every 10 s and 1 s after macOS adds a multi-touch device. At a wake from
// deep sleep macOS makes the built-in trackpad's device anew (same Multitouch
// ID, new IORegistry entry) and the old one never sends frames again, so a
// newer entry with the same ID takes the old one's place.
static uint64_t mtEntry(MTDeviceRef d) {
  io_service_t svc = mt.service(d);
  uint64_t e = 0;
  if (!svc || IORegistryEntryGetRegistryEntryID(svc, &e) != KERN_SUCCESS) return 0;
  return e;
}

static int takesOver(MTDeviceRef held, uint64_t heldEntry, MTDeviceRef d, uint64_t entry) {
  if (held == d) return 0;
  if (heldEntry && entry && heldEntry != entry) return entry > heldEntry;
  return !mt.isRunning(held);
}

static void stopDevice(MTDeviceRef d) {
  mt.unregisterCb(d, frameCb);
  if (mt.isRunning(d)) mt.stop(d);
  CFRelease(d);
}

static int startDevice(MTDeviceRef d, const char *what) {
  mt.registerCb(d, frameCb);
  int err = mt.start(d, 0);
  if (!err) return 1;
  static time_t said;
  time_t now = time(NULL);
  if (now - said >= 60) { said = now; pg_log("MultitouchSupport did not start the %s (0x%08x)", what, (unsigned)err); }
  return 0;
}

static void startTrackpads(void) {
  if (!loadMultitouch()) return;
  CFArrayRef list = mt.createList();
  for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
    MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
    int w = 0, h = 0;
    if (mt.dimensions(d, &w, &h) != 0) w = h = 0;
    int builtIn = mt.isBuiltIn(d);
    if (!builtIn && w < 10000) continue;   // a Magic Mouse or other small surface
    uint64_t id = 0, entry = mtEntry(d);
    mt.deviceID(d, &id);
    int slot = -1, known = 0, renewed = 0, sized = 0;
    MTDeviceRef old = NULL;
    pthread_mutex_lock(&padLock);
    for (int k = 0; k < nPads; k++) {
      if (pads[k].id != id) continue;
      if (takesOver(pads[k].dev, pads[k].entry, d, entry)) slot = k; else known = 1;
    }
    if (!known && slot >= 0) { old = pads[slot].dev; renewed = pads[slot].entry != entry; }
    if (!known && slot < 0 && nPads < MAX_PADS) slot = nPads++;
    if (!known && slot >= 0) {
      CFRetain(d);   // held until let go (stopDevice)
      pads[slot].dev = d; pads[slot].id = id; pads[slot].entry = entry; pads[slot].fingers = 0;
      pads[slot].w = w > 0 ? w : 15600; pads[slot].h = h > 0 ? h : 9600;
      // A new device for a trackpad held keeps the one touched last.
      if (activePad < 0 || (!old && builtIn)) {
        activePad = slot;
        sized = tpW != pads[slot].w || tpH != pads[slot].h;
        tpW = pads[slot].w; tpH = pads[slot].h;
      }
    }
    int pw = slot >= 0 ? pads[slot].w : 0, ph = slot >= 0 ? pads[slot].h : 0;
    pthread_mutex_unlock(&padLock);
    if (sized) sendSize();
    if (known || slot < 0) continue;
    if (old) stopDevice(old);   // its device is gone (or did not start)
    if (!startDevice(d, builtIn ? "built-in trackpad" : "Magic Trackpad")) continue;
    pg_log("trackpad: %s%s, %d x %d mm", builtIn ? "built-in" : "Magic Trackpad", renewed ? " again" : "", pw / 100, ph / 100);
  }
  if (list) CFRelease(list);   // the devices taken are held on their own
}

// Every trackpad let go; the callbacks run on MultitouchSupport's thread, so
// the table is emptied under its lock first, the devices stopped outside it.
static void stopTrackpads(void) {
  if (!loadMultitouch()) return;
  MTDeviceRef p[MAX_PADS];
  pthread_mutex_lock(&padLock);
  int np = nPads;
  for (int i = 0; i < np; i++) p[i] = pads[i].dev;
  nPads = 0; activePad = -1;
  pthread_mutex_unlock(&padLock);
  for (int i = 0; i < np; i++) stopDevice(p[i]);
  fingers = 0;
}

static IONotificationPortRef mtPort;
static io_iterator_t mtIter;
static int lookQueued;   // worker thread
static int inputPaused;  // worker thread: no tap, no trackpads until the permission is back
static void multitouchAdded(void *refcon, io_iterator_t it) {
  (void)refcon;
  io_object_t o;
  while ((o = IOIteratorNext(it))) IOObjectRelease(o);   // all of them, or no next notice comes
  // Not at once: MTDeviceStart waits for a busy device (up to 5 s, on the tap's thread).
  if (lookQueued) return;
  lookQueued = 1;
  afterOnLoop(1.0, ^{ lookQueued = 0; if (!inputPaused) startTrackpads(); });
}
static void watchMultitouch(void) {
  mtPort = IONotificationPortCreate(MACH_PORT_NULL);
  if (!mtPort) return;
  CFRunLoopAddSource(loop, IONotificationPortGetRunLoopSource(mtPort), kCFRunLoopCommonModes);
  if (IOServiceAddMatchingNotification(mtPort, kIOFirstMatchNotification, IOServiceMatching("AppleMultitouchDevice"),
                                       multitouchAdded, NULL, &mtIter) != KERN_SUCCESS) {
    mtIter = 0;
    return;
  }
  io_object_t o;
  while ((o = IOIteratorNext(mtIter))) IOObjectRelease(o);   // the ones there now; arms it
}
static void unwatchMultitouch(void) {
  if (mtIter) { IOObjectRelease(mtIter); mtIter = 0; }
  if (mtPort) {
    CFRunLoopRemoveSource(loop, IONotificationPortGetRunLoopSource(mtPort), kCFRunLoopCommonModes);
    IONotificationPortDestroy(mtPort);
    mtPort = NULL;
  }
}

// ---- capture: the target is frontmost with a full-screen window ----
// A full-screen window is a normal window (layer 0) that spans a display's
// width and nearly its height (it may sit below the notch strip).
static int targetFullScreen(pid_t pid) {
  CFArrayRef wins = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  if (!wins) return 0;
  CGDirectDisplayID ds[16]; uint32_t nd = 0;
  if (CGGetActiveDisplayList(16, ds, &nd) != kCGErrorSuccess) nd = 0;
  int found = 0;
  for (CFIndex i = 0; i < CFArrayGetCount(wins) && !found; i++) {
    CFDictionaryRef w = CFArrayGetValueAtIndex(wins, i);
    int owner = 0, layer = -1; CGRect r;
    CFNumberRef on = CFDictionaryGetValue(w, kCGWindowOwnerPID), ln = CFDictionaryGetValue(w, kCGWindowLayer);
    CFDictionaryRef bounds = CFDictionaryGetValue(w, kCGWindowBounds);
    if (!on || !ln || !bounds) continue;
    CFNumberGetValue(on, kCFNumberIntType, &owner);
    CFNumberGetValue(ln, kCFNumberIntType, &layer);
    if (owner != pid || layer != 0) continue;
    if (!CGRectMakeWithDictionaryRepresentation(bounds, &r)) continue;
    for (uint32_t d = 0; d < nd; d++) {
      CGRect b = CGDisplayBounds(ds[d]);
      if (fabs(r.size.width - b.size.width) < 2 && r.size.height >= b.size.height - 80 &&
          fabs(r.origin.x - b.origin.x) < 2) { found = 1; break; }
    }
  }
  CFRelease(wins);
  return found;
}

static pid_t frontPid(void) {
  ProcessSerialNumber psn; pid_t pid = 0;
  if (GetFrontProcess(&psn) != noErr || GetProcessPID(&psn, &pid) != noErr) return 0;
  return pid;
}

// macOS's Three Finger Drag (both trackpad domains; System Settings writes both).
static void threeDragLook(void) {
  static double looked = -1;
  double now = monoNow();
  if (looked >= 0 && now - looked < 1.0) return;
  looked = now;
  CFStringRef domains[] = { CFSTR("com.apple.AppleMultitouchTrackpad"), CFSTR("com.apple.driver.AppleBluetoothMultitouch.trackpad") };
  int on = 0;
  for (size_t i = 0; i < sizeof domains / sizeof *domains && !on; i++) {
    CFPreferencesAppSynchronize(domains[i]);
    Boolean valid = false;
    on = CFPreferencesGetAppBooleanValue(CFSTR("TrackpadThreeFingerDrag"), domains[i], &valid) && valid;
  }
  if (on != threeDrag) pg_log("three finger drag %s", on ? "on: four and more fingers go to the VM" : "off");
  threeDrag = on;
}

// ---- the event tap ----
// An active tap at the head of the HID chain. QEMU's own tap (made when it
// starts, usually after ours) then sits ahead of it, so the tap is made again
// each time the target comes to the front full screen. The new tap goes in
// before the old one is removed: no gap without one. Never without
// Accessibility: macOS would stop taking the events it hands back and every
// key and click on the Mac would be held.
static CGEventRef tapCb(CGEventTapProxy p, CGEventType type, CGEventRef e, void *u);
// Accessibility is answered in the process (fast); Input Monitoring and
// "control the computer" ask tccd (slow): asked off the worker every 10 s and
// their last answer used in between.
static int slowPermNow(void) {
  return (CGPreflightListenEventAccess() ? PERM_IM : 0) | (CGPreflightPostEventAccess() ? PERM_POST : 0);
}
static volatile int slowPerm;
static int permNow(void) { return (AXIsProcessTrusted() ? PERM_AX : 0) | slowPerm; }
static int tapPerm;    // the permissions the tap was made with
static int seenPerm;   // every permission a tap was made with

// The watchdog guards the tap: when the worker (the tap's callback) has not
// answered for a second, the tap is disabled so it never holds the Mac's input.
static pthread_mutex_t watchLock = PTHREAD_MUTEX_INITIALIZER;
static CFMachPortRef watchTap;   // under watchLock
static double watchAsked;        // under watchLock: when the worker was asked to answer (0: it did)
static pthread_t watchdog;
static void watchTapSet(CFMachPortRef t) {
  pthread_mutex_lock(&watchLock); watchTap = t; watchAsked = 0; pthread_mutex_unlock(&watchLock);
}

static int installTap(void) {
  int perm = permNow();
  if (!(perm & PERM_AX)) return 0;
  if ((seenPerm & PERM_POST) && !(perm & PERM_POST)) return 0;
  CFMachPortRef newTap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, tapMask, tapCb, NULL);
  if (!newTap) return 0;
  CFRunLoopSourceRef newSource = CFMachPortCreateRunLoopSource(NULL, newTap, 0);
  if (!newSource) { CFMachPortInvalidate(newTap); CFRelease(newTap); return 0; }
  CFRunLoopAddSource(loop, newSource, kCFRunLoopCommonModes);
  if (tapSource) {
    CFRunLoopRemoveSource(loop, tapSource, kCFRunLoopCommonModes);
    CFRunLoopSourceInvalidate(tapSource);
    CFRelease(tapSource);
  }
  watchTapSet(newTap);
  if (tapPort) { CFMachPortInvalidate(tapPort); CFRelease(tapPort); }
  tapPort = newTap;
  tapSource = newSource;
  tapPerm = perm;
  seenPerm |= perm;
  return 1;
}

static void removeTap(void) {
  if (!tapPort) return;
  watchTapSet(NULL);
  CGEventTapEnable(tapPort, false);
  CFRunLoopRemoveSource(loop, tapSource, kCFRunLoopCommonModes);
  CFRunLoopSourceInvalidate(tapSource);
  CFRelease(tapSource);
  CFMachPortInvalidate(tapPort);
  CFRelease(tapPort);
  tapPort = NULL;
  tapSource = NULL;
}

static int permitted(int perm) { return (perm & PERM_AX) && (perm & tapPerm) == tapPerm; }

// No tap and no trackpads: macOS has the keyboard, clicks and gestures.
static void pauseInput(void) {
  removeTap();
  inputPaused = 1;
  stopTrackpads();
  if (capturing) { capturing = 0; pg_log("capture off"); sendState("off"); }
}

// A tap macOS (or the watchdog) disabled is removed, never enabled again; a
// new one comes after a hold-off (2 s, then 30 s), and after three in 10
// minutes none until macOS's Accessibility list changes.
#define TAP_PARK 3
#define TAP_FORGET 600.0
static const double tapHoldOff[TAP_PARK - 1] = { 2, 30 };
static double tapStrikes[TAP_PARK];
static int nStrikes, tapParked;
static double tapUntil;
static int tapMayCreate(void) { return !tapParked && monoNow() >= tapUntil; }
static void rearmTap(const char *why);
static double tapStrike(double now) {
  int k = 0;
  for (int i = 0; i < nStrikes; i++) if (now - tapStrikes[i] < TAP_FORGET) tapStrikes[k++] = tapStrikes[i];
  if (k == TAP_PARK) { memmove(tapStrikes, tapStrikes + 1, (TAP_PARK - 1) * sizeof *tapStrikes); k--; }
  tapStrikes[k++] = now;
  nStrikes = k;
  if (k >= TAP_PARK) { tapParked = 1; return -1; }
  tapUntil = now + tapHoldOff[k - 1];
  return tapHoldOff[k - 1];
}
static void tapDisabled(const char *by) {
  if (!tapPort) return;
  double h = tapStrike(monoNow());
  if (h < 0) {
    pauseInput();
    pg_log("event tap disabled (%s) %d times in 10 minutes: let go until macOS's Accessibility list changes", by, TAP_PARK);
    return;
  }
  removeTap();
  pg_log("event tap disabled (%s): removed, a new one in %.0f s", by, h);
  afterOnLoop(h, ^{ if (!tapPort && !inputPaused && tapMayCreate()) rearmTap("hold-off over"); });
}

static void *watchdogMain(void *u) {
  (void)u;
  while (!quit) {
    usleep(250000);
    pthread_mutex_lock(&watchLock);
    int step = 0;
    if (watchTap) {
      double now = monoNow();
      if (watchAsked == 0) { watchAsked = now; step = 1; }
      else if (now - watchAsked > 1.0) step = 2;
    }
    if (step == 2) {
      CFMachPortRef t = watchTap;
      watchTap = NULL;   // once: the worker removes it when it is back
      CGEventTapEnable(t, false);
      pthread_mutex_unlock(&watchLock);
      pg_log("the tap's thread did not answer for 1 s: event tap disabled");
      continue;
    }
    pthread_mutex_unlock(&watchLock);
    if (step == 1) onLoop(^{ pthread_mutex_lock(&watchLock); watchAsked = 0; pthread_mutex_unlock(&watchLock); });
  }
  return NULL;
}

static void checkPermissions(void) {
  if (tapPort && !inputPaused) {
    if (permitted(permNow())) return;
    pauseInput();
    pg_log("permission taken away: event tap removed, trackpads let go; waiting for it to come back");
  } else if (inputPaused && !tapParked && installTap()) {
    inputPaused = 0;
    startTrackpads();
    pg_log("event tap and trackpads on again");
  }
}

static int slowAsking;   // worker thread
static void refreshSlowPerms(void) {
  if (slowAsking) return;
  slowAsking = 1;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    int p = slowPermNow();
    onLoop(^{ slowAsking = 0; slowPerm = p; checkPermissions(); });
  });
}

static void rearmTap(const char *why) {
  if (inputPaused || !tapMayCreate()) return;
  if (installTap()) pg_log("event tap made again (%s)", why);
  else if (!permitted(permNow())) checkPermissions();
}

// macOS's Accessibility list changed (delivered on whichever thread runs the
// distributed center's run loop): look now on the worker, and once more later.
static void accessibilityChanged(CFNotificationCenterRef c, void *o, CFNotificationName n, const void *obj, CFDictionaryRef info) {
  (void)c; (void)o; (void)n; (void)obj; (void)info;
  onLoop(^{
    nStrikes = 0; tapUntil = 0; tapParked = 0;
    checkPermissions(); refreshSlowPerms();
    afterOnLoop(0.3, ^{ checkPermissions(); refreshSlowPerms(); });
  });
}

// ---- the capture check (worker, every 0.2 s) ----
static pid_t tapFront;   // the target pid the tap was last made again for (0: not in front)
static void updateCapture(CFRunLoopTimerRef t, void *info) {
  (void)t; (void)info;
  threeDragLook();
  pid_t pid = frontPid();
  int front = pid > 0 && pid == targetPid && targetFullScreen(pid);
  if (front && tapFront != pid) rearmTap("the VM came to the front");
  tapFront = front ? pid : 0;
  if (!front) escaped = 0;   // re-armed once the VM is left
  frontIsTarget = front;
  int now = front && !escaped && !inputPaused;
  if (now != capturing) {
    capturing = now;
    pg_log("capture %s", now ? "ON" : "off");
    sendState(now ? "on" : "off");
  }
  // Invalidated or disabled (by macOS or the watchdog): without the
  // permissions it goes; a disabled one is removed, never enabled again.
  if (tapPort && (!CFMachPortIsValid(tapPort) || !CGEventTapIsEnabled(tapPort))) {
    if (!permitted(permNow())) checkPermissions();
    else if (!CFMachPortIsValid(tapPort)) rearmTap("macOS invalidated it");
    else tapDisabled("found disabled");
  }
}

static void permissionTimer(CFRunLoopTimerRef t, void *info) {
  (void)t; (void)info;
  static int ticks;
  checkPermissions();
  if (++ticks % 10 == 0 || (inputPaused && (permNow() & PERM_AX))) refreshSlowPerms();
}

static void retryTrackpads(CFRunLoopTimerRef t, void *info) {
  (void)t; (void)info;
  if (!inputPaused) startTrackpads();
}

// ---- Cmd as Super ----
// While capturing, every Cmd+key goes to the guest instead of macOS, which
// types it as Super+key: "K <linux keycode> <0 up|1 down|2 repeat>".
static unsigned short macToLinux[128];
static unsigned char forwarded[128];

static void initKeymap(void) {
  // Physical keys (kVK_* -> KEY_*); the guest's own layout gives them meaning.
  static const unsigned short pairs[][2] = {
    {0,30},{1,31},{2,32},{3,33},{4,35},{5,34},{6,44},{7,45},{8,46},{9,47},{11,48},{12,16},{13,17},{14,18},
    {15,19},{16,21},{17,20},{18,2},{19,3},{20,4},{21,5},{22,7},{23,6},{24,13},{25,10},{26,8},{27,12},{28,9},
    {29,11},{30,27},{31,24},{32,22},{33,26},{34,23},{35,25},{36,28},{37,38},{38,36},{39,40},{40,37},{41,39},
    {42,43},{43,51},{44,53},{45,49},{46,50},{47,52},{48,15},{49,57},{51,14},{53,1},{76,96},
    {96,63},{97,64},{98,65},{99,61},{100,66},{101,67},{103,87},{109,68},{111,88},{118,62},{120,60},{122,59},
    {115,102},{116,104},{117,111},{119,107},{121,109},{123,105},{124,106},{125,108},{126,103}};
  for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) macToLinux[pairs[i][0]] = pairs[i][1];
  // The key left of 1 and the extra key beside left Shift swap places on ISO keyboards.
  int iso = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
  macToLinux[10] = iso ? 41 : 86;   // kVK_ISO_Section
  macToLinux[50] = iso ? 86 : 41;   // kVK_ANSI_Grave
}

static void sendKey(int code, int val) {
  char b[24]; int n = snprintf(b, sizeof b, "K %d %d\n", code, val); sendLine(b, (size_t)n);
}

static void forwardKey(int kc, CGEventFlags f, int val) {
  if (val == 1) {
    sendKey(125, 1);                                         // Super
    if (f & kCGEventFlagMaskShift) sendKey(42, 1);
    if (f & kCGEventFlagMaskControl) sendKey(29, 1);
    if (f & kCGEventFlagMaskAlternate) sendKey(56, 1);
  }
  sendKey(macToLinux[kc], val);
  if (val == 0) { sendKey(56, 0); sendKey(29, 0); sendKey(42, 0); sendKey(125, 0); }
}

// ---- event tap callback (worker thread) ----
// Ctrl+Option+Esc exactly (with Shift or Cmd added it is any other key).
static int escapeCombo(int kc, CGEventFlags f) {
  const CGEventFlags ctrlOpt = kCGEventFlagMaskControl | kCGEventFlagMaskAlternate;
  return kc == ESC_KEYCODE && (f & (ctrlOpt | kCGEventFlagMaskCommand | kCGEventFlagMaskShift)) == ctrlOpt;
}
static int swallowEscUp;

static CGEventRef tapCb(CGEventTapProxy p, CGEventType type, CGEventRef e, void *u) {
  (void)p; (void)u;
  if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
    const char *by = type == kCGEventTapDisabledByTimeout ? "macOS's timeout" : "user input";
    CFMachPortRef was = tapPort;
    if (permitted(permNow())) { onLoop(^{ if (tapPort == was) tapDisabled(by); }); refreshSlowPerms(); }
    else onLoop(^{ checkPermissions(); });
    return e;
  }
  if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
    int kc = (int)CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode);
    CGEventFlags f = CGEventGetFlags(e);
    int combo = escapeCombo(kc, f);
    // A plain Esc going down: the combo's Esc up we meant to eat went
    // elsewhere (QEMU's tap ahead of ours), so this press keeps its own up.
    if (kc == ESC_KEYCODE && type == kCGEventKeyDown && !combo) swallowEscUp = 0;
    if (kc == ESC_KEYCODE && type == kCGEventKeyUp && swallowEscUp) { swallowEscUp = 0; return NULL; }
    if (combo && frontIsTarget) {
      if (type == kCGEventKeyUp) return e;
      if (CGEventGetIntegerValueField(e, kCGKeyboardEventAutorepeat)) return NULL;
      // Only the real keyboard: apps that post key events must not hand the
      // trackpad back to macOS.
      if (CGEventGetIntegerValueField(e, kCGEventSourceStateID) != kCGEventSourceStateHIDSystemState) return e;
      // Captured: let go to macOS. Released by the combo: capture again.
      escaped = capturing;
      int now = !escaped && !inputPaused;
      if (now != capturing) {
        capturing = now;
        pg_log("escape combo: capture %s", now ? "ON" : "off");
        sendState(now ? "on" : "esc");
      }
      swallowEscUp = 1;
      return NULL;
    }
    if (kc >= 0 && kc < 128 && macToLinux[kc]) {
      if (type == kCGEventKeyDown && capturing && (f & kCGEventFlagMaskCommand)) {
        forwardKey(kc, f, CGEventGetIntegerValueField(e, kCGKeyboardEventAutorepeat) ? 2 : 1);
        forwarded[kc] = 1;
        return NULL;
      }
      if (type == kCGEventKeyUp && forwarded[kc]) { forwarded[kc] = 0; forwardKey(kc, f, 0); return NULL; }
    }
    return e;
  }
  // A gesture event. A three-finger touch with Three Finger Drag is macOS's drag.
  if (threeDrag && fingers == 3) return e;
  return capturing ? NULL : e;
}

// ---- the worker ----
static pthread_mutex_t readyLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t readyCond = PTHREAD_COND_INITIALIZER;
static int readyState;   // 0: starting, 1: running, -1: failed

static void setReady(int s) {
  pthread_mutex_lock(&readyLock); readyState = s; pthread_cond_signal(&readyCond); pthread_mutex_unlock(&readyLock);
}

static CFRunLoopTimerRef addTimer(double first, double every, double tolerance, CFRunLoopTimerCallBack cb) {
  CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + first, every, 0, 0, cb, NULL);
  CFRunLoopTimerSetTolerance(t, tolerance);
  CFRunLoopAddTimer(loop, t, kCFRunLoopCommonModes);
  return t;
}

static void *workerMain(void *u) {
  (void)u;
  pthread_setname_np("pingouin-gestures");
  pthread_mutex_lock(&loopLock);
  loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
  pthread_mutex_unlock(&loopLock);
  slowPerm = slowPermNow();
  if (!installTap()) {
    pg_log("cannot make the event tap (Accessibility?)");
    setReady(-1);
    return NULL;
  }
  if (!loadMultitouch()) pg_log("MultitouchSupport not available: keys only");
  startTrackpads();
  watchMultitouch();
  CFRunLoopTimerRef timers[] = {
    addTimer(0, 0.2, 0.02, updateCapture),   // the capture check
    addTimer(1, 1, 0.1, permissionTimer),    // a permission taken away: let go at once
    addTimer(10, 10, 1, retryTrackpads),     // a Magic Trackpad connected later, or again
  };
  CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), &loop, accessibilityChanged,
                                  CFSTR("com.apple.accessibility.api"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
  pthread_create(&watchdog, NULL, watchdogMain, NULL);
  sendSize();
  pg_log("running for pid %d (escape: Ctrl+Option+Esc)", (int)targetPid);
  setReady(1);

  while (!quit) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);

  CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(), &loop, CFSTR("com.apple.accessibility.api"), NULL);
  for (size_t i = 0; i < sizeof timers / sizeof *timers; i++) { CFRunLoopTimerInvalidate(timers[i]); CFRelease(timers[i]); }
  pthread_join(watchdog, NULL);
  removeTap();
  stopTrackpads();
  unwatchMultitouch();
  if (capturing) { capturing = 0; sendState("off"); }
  return NULL;
}

// ---- public API ----
int pg_permissions(void) {
  return (AXIsProcessTrusted() ? PG_PERM_ACCESSIBILITY : 0) | (CGPreflightListenEventAccess() ? PG_PERM_INPUT_MONITORING : 0);
}

void pg_request_permissions(void) {
  int perm = pg_permissions();
  if (!(perm & PG_PERM_ACCESSIBILITY)) {
    const void *keys[] = { kAXTrustedCheckOptionPrompt }, *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
  }
  if (!(perm & PG_PERM_INPUT_MONITORING)) CGRequestListenEventAccess();
}

static void releaseLoop(void) {
  pthread_mutex_lock(&loopLock);
  if (loop) CFRelease(loop);
  loop = NULL;
  pthread_mutex_unlock(&loopLock);
}

int pg_start(pid_t target_pid, pg_send_fn send, void *ctx) {
  if (started) pg_stop();
  int perm = pg_permissions();
  if ((perm & (PG_PERM_ACCESSIBILITY | PG_PERM_INPUT_MONITORING)) != (PG_PERM_ACCESSIBILITY | PG_PERM_INPUT_MONITORING)) {
    pg_log("missing permission:%s%s", perm & PG_PERM_ACCESSIBILITY ? "" : " Accessibility",
           perm & PG_PERM_INPUT_MONITORING ? "" : " Input Monitoring");
    return -1;
  }
  pthread_mutex_lock(&sendLock); sendFn = send; sendCtx = ctx; pthread_mutex_unlock(&sendLock);
  targetPid = target_pid;
  quit = 0; slowAsking = 0; tapFront = 0; frontIsTarget = 0; escaped = 0; capturing = 0; inputPaused = 0; lookQueued = 0;
  forwarding = 0; pinchArmed = 0; swallowEscUp = 0;
  nStrikes = 0; tapParked = 0; tapUntil = 0; tapPerm = 0; seenPerm = 0;
  memset(forwarded, 0, sizeof forwarded);
  initKeymap();
  tapMask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
  // rotate, gesture begin/end, gesture, magnify, swipe, smart magnify, pressure
  static const int gestureTypes[] = { 18, 19, 20, 29, 30, 31, 32, 34 };
  for (size_t i = 0; i < sizeof gestureTypes / sizeof *gestureTypes; i++) tapMask |= (CGEventMask)1 << gestureTypes[i];

  readyState = 0;
  if (pthread_create(&worker, NULL, workerMain, NULL)) return -1;
  pthread_mutex_lock(&readyLock);
  while (readyState == 0) pthread_cond_wait(&readyCond, &readyLock);
  int ok = readyState > 0;
  pthread_mutex_unlock(&readyLock);
  if (!ok) {
    pthread_join(worker, NULL);
    releaseLoop();
    pthread_mutex_lock(&sendLock); sendFn = NULL; sendCtx = NULL; pthread_mutex_unlock(&sendLock);
    return -1;
  }
  started = 1;
  return 0;
}

void pg_hello(void) {
  if (started) sendSize();
}

void pg_stop(void) {
  if (!started) return;
  pthread_mutex_lock(&loopLock);
  quit = 1;
  if (loop) { CFRunLoopStop(loop); CFRunLoopWakeUp(loop); }
  pthread_mutex_unlock(&loopLock);
  pthread_join(worker, NULL);
  releaseLoop();
  started = 0;
  pthread_mutex_lock(&sendLock); sendFn = NULL; sendCtx = NULL; pthread_mutex_unlock(&sendLock);
}
