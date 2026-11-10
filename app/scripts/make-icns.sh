#!/bin/bash
# Makes Resources/Pingouin.icns (the flat icon: macOS 15 and older, and builds
# without Xcode's actool) from a 1024 x 1024 PNG, e.g. Icon Composer's export
# of Resources/Pingouin.icon.   scripts/make-icns.sh icon-1024.png [Name]
# Name: the icns's name in Resources (default Pingouin; VM: the VM windows' icon).
set -euo pipefail
src=${1:?usage: scripts/make-icns.sh icon-1024.png [Name]}
name=${2:-Pingouin}
app_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
set=$tmp/$name.iconset
mkdir "$set"
for s in 16 32 128 256 512; do
  sips -z $s $s "$src" --out "$set/icon_${s}x${s}.png" >/dev/null
  sips -z $((s * 2)) $((s * 2)) "$src" --out "$set/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$set" -o "$app_dir/Resources/$name.icns"
echo "$app_dir/Resources/$name.icns"
