#!/bin/bash
# The app menu's memory lines (pingouin-cocoa-graphics-memory.patch): the
# patch's ui/pingouin-gpu-memory.h goes into an empty folder, and
# test-gpu-memory-menu.c runs against it. No QEMU, no display. CI and the
# runtime build run it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-gpu-memory-menu.XXXXXX")
trap 'rm -rf "$work"' EXIT
# Only the header's part of the patch (the rest changes ui/cocoa.m).
awk '/^diff --git /{on = ($0 ~ /ui\/pingouin-gpu-memory\.h/)} on' \
  "$patches/pingouin-cocoa-graphics-memory.patch" > "$work/header.patch"
patch -s -d "$work" -p1 -f -i "$work/header.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" \
  "$here/test-gpu-memory-menu.c" -o "$work/test-gpu-memory-menu"
"$work/test-gpu-memory-menu"
