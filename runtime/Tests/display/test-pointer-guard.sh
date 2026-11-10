#!/bin/bash
# The full-screen pointer guard's maths (pingouin-cocoa-pointer-guard.patch):
# the patch makes ui/pingouin-pointer-guard.h in an empty folder, and
# test-pointer-guard.c runs against it. No QEMU, no display. CI and the
# runtime build run it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-pointer-guard.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-pointer-guard.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" \
  "$here/test-pointer-guard.c" -lm -o "$work/test-pointer-guard"
"$work/test-pointer-guard"
