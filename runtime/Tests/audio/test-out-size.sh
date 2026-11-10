#!/bin/bash
# The size of QEMU's SDL playback buffers for the Mac's output (#548):
# qemu-sdl-audio-out-size-logic.patch makes audio/pingouin-sdl-out-size.h in
# an empty folder and test-out-size.c runs against it. With audio/sdlaudio.c
# as argument (the runtime build, after qemu-sdl-audio-out-size.patch): also
# that the playback open uses the rules.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-out-size.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/qemu-sdl-audio-out-size-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/audio" "$here/test-out-size.c" -o "$work/test-out-size"
"$work/test-out-size"

(( $# )) || exit 0
s=$1
fail() { echo "test-out-size: $s: $*" >&2; exit 1; }
body() { awk -v h="$1" 'index($0, h) == 1 && !/;$/ {on=1} on {print} on && /^}$/ {exit}' "$s"; }
grep -q '#include "pingouin-sdl-out-size.h"' "$s" || fail "the rules are not included"
open=$(body 'static void *sdl_out_open_thread(void *opaque)')
start=$(body 'static void sdl_out_start_open_closing(SDLVoiceOut *sdl, char *device_name,')
want=$(body 'static unsigned sdl_out_want(SDLVoiceOut *sdl)')
route=$(body 'static void sdl_refresh_route_Out(SDLVoiceOut *sdl)')
[[ -n $open && -n $start && -n $want && -n $route ]] || fail "a piece of the wiring is missing"
# A refused start tries the next size on the same open, at once.
grep -q 'pingouin_sdl_out_start_refused(' <<<"$open" && grep -q 'pingouin_sdl_out_next(' <<<"$open" ||
  fail "a refused start does not try a larger size"
# The old device is closed before the new size opens (SDL3 shares an open physical device).
awk '/SDL_CloseAudioDevice\(op->close_first\)/ && !c {c = NR} /sdl_open\(/ && !o {o = NR} END {exit !(c && o && c < o)}' <<<"$open" ||
  fail "a resize opens before the old device is closed"
# A resize's close of the old device is timed too: a close that hangs (coreaudiod wedged) makes the
# voice drop the sound and say so, as a hung open does.
awk '/op->started = g_get_monotonic_time\(\);/ && !t {t = NR} /SDL_CloseAudioDevice\(op->close_first\)/ && !c {c = NR}
     END {exit !(t && c && t < c)}' <<<"$open" || fail "a resize's close of the old device is not timed"
# Every open is sized from the voice's size, the launcher's and a refused one.
grep -q 'pingouin_sdl_out_frames(' <<<"$want" && grep -q 'op->spec.samples = sdl_out_want(sdl);' <<<"$start" ||
  fail "the open is not sized"
# After every size was refused, no start until the Mac's output changes (the launcher's new size file;
# 30 s without one): each failed start may leave coreaudiod silent.
grep -q 'sdl->refused_until = pingouin_sdl_out_hold_until(' <<<"$(body 'static void sdl_out_poll(SDLVoiceOut *sdl)')" &&
  grep -q 'sdl->refused_until' <<<"$start" || fail "a refusing output is started again while it stays"
# Each size gets its own time limit: slow refusals in a row are not a device that does not answer.
grep -q 'op->started = g_get_monotonic_time();' <<<"$(awk '/for \(;;\) \{/,/^    }$/' <<<"$open")" ||
  fail "the sizes tried in a row share one time limit"
# A new size file is another Mac output: what the last one refused no longer counts.
hint=$(body 'static void sdl_out_hint_read(SDLVoiceOut *sdl)')
grep -q 'sdl->out_floor = 0;' <<<"$hint" && grep -q 'sdl->refused_until = 0;' <<<"$hint" ||
  fail "a refusal of the last output still counts for a new one"
# A new size that did not open (its old device closed) is tried again while the guest plays.
grep -q 'op->close_first ? now + SDL_OUT_RETRY_US' <<<"$(body 'static void sdl_out_poll(SDLVoiceOut *sdl)')" ||
  fail "a failed resize leaves the VM silent until its next stream"
# The launcher's size is followed while the device runs.
grep -q 'sdl_out_hint' <<<"$route" || fail "a change of the Mac's output is not followed"
echo "test-out-size: wiring ok"
