#!/bin/bash
# macOS's shortcuts while the VM has the keyboard (pingouin-cocoa-shortcuts-logic.patch):
# the patch makes ui/pingouin-shortcuts.h in an empty folder; test-shortcuts.c checks
# it against mac-shortcuts.tsv (macOS's list with its keys, F-keys with every
# modifier, the app-menu chords) and against this Mac's own list read from the
# window server (live-shortcuts.c; skipped where there is none). No QEMU, no
# display. CI and the runtime build run it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-shortcuts.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-shortcuts-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" "$here/test-shortcuts.c" -o "$work/test-shortcuts"
live=()
if cc -o "$work/live-shortcuts" "$here/live-shortcuts.c" -framework CoreGraphics 2>/dev/null &&
   "$work/live-shortcuts" > "$work/live.tsv" 2>/dev/null && [[ -s $work/live.tsv ]]; then
  live=("$work/live.tsv")
else
  echo "skip this Mac's own list (no window server here)"
fi
"$work/test-shortcuts" "$here/mac-shortcuts.tsv" "${live[@]}"
