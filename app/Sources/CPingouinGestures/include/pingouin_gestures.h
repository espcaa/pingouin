// Mac trackpad + Cmd keys -> the VM's pingouind, as text lines.
// Lines are the same protocol pingouind reads:
//   F <n> [<id> <x> <y> <size>]...   finger frame (x/y 0..1, y down), 3+ fingers and pinch
//   O <natural> <w> <h>              scroll direction, trackpad size (1/100 mm)
//   K <linux keycode> <0|1|2>        a key for the VM's virtual keyboard (Cmd as Super)
//   S <on|off|esc>                   capture state
#ifndef PINGOUIN_GESTURES_H
#define PINGOUIN_GESTURES_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called on an internal thread with one complete line ending in '\n'.
typedef void (*pg_send_fn)(const char *line, size_t len, void *ctx);

enum {
  PG_PERM_ACCESSIBILITY    = 1 << 0,
  PG_PERM_INPUT_MONITORING = 1 << 1,
};

// Which permissions this process has right now (PG_PERM_* bits).
int pg_permissions(void);
// Shows macOS's prompts for whichever permissions are missing.
void pg_request_permissions(void);

// Starts reading the trackpad. Capture is on only while a window of
// target_pid is frontmost and full screen; ctrl+option+esc releases it.
// Returns 0, or -1 when a needed permission is missing.
int pg_start(pid_t target_pid, pg_send_fn send, void *ctx);
// Re-sends the "O" line (call when the VM side (re)connects).
void pg_hello(void);
void pg_stop(void);

#ifdef __cplusplus
}
#endif
#endif
