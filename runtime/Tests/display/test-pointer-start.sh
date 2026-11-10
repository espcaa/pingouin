#!/bin/bash
# When the VM takes the pointer without a click, and when the Mac's cursor
# hides (pingouin-cocoa-pointer-start-logic.patch): the patch makes
# ui/pingouin-pointer-start.h in an empty folder, and test-pointer-start.c runs
# against it. No QEMU, no display. CI and the runtime build run it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-pointer-start.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-pointer-start-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" \
  "$here/test-pointer-start.c" -o "$work/test-pointer-start"
"$work/test-pointer-start"
