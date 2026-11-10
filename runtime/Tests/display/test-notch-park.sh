#!/bin/bash
# Where the guest's pointer goes when the Mac's leaves up into Omanotch's
# strip, and the one-cursor hand-off at that edge
# (pingouin-cocoa-notch-park-logic.patch): the patch makes
# ui/pingouin-notch-park.h in an empty folder, and test-notch-park.c runs
# against it. No QEMU, no display. CI and the runtime build run it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
patches=$(cd "$here/../../patches" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/pingouin-notch-park.XXXXXX")
trap 'rm -rf "$work"' EXIT
patch -s -d "$work" -p1 -f -i "$patches/pingouin-cocoa-notch-park-logic.patch"
cc -std=c11 -Wall -Wextra -Werror -I"$work/ui" \
  "$here/test-notch-park.c" -o "$work/test-notch-park"
"$work/test-notch-park"
