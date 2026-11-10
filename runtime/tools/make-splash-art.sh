#!/bin/bash
# Makes ui/pingouin-splash-art.h (the boot splash's penguin) for
# patches/pingouin-splash-penguin.patch from app/Resources/icon/penguin.png.
# Prints the header; paste it into the patch's new-file hunk.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sips -z 768 502 "$here/../app/Resources/icon/penguin.png" --out "$tmp/penguin-768.png" >/dev/null
cat <<'H'
/*
 * Pingouin boot splash: the penguin of the app's icon (app/Resources/icon/
 * penguin.png at 502 x 768, RGBA PNG). Made by runtime/tools/make-splash-art.sh.
 */
#ifndef PINGOUIN_SPLASH_ART_H
#define PINGOUIN_SPLASH_ART_H

H
(cd "$tmp" && xxd -i -n pingouin_penguin_png penguin-768.png) |
  sed 's/^unsigned char/static const unsigned char/; s/^unsigned int/static const unsigned int/'
printf '\n#endif\n'
