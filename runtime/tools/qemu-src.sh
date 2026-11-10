#!/bin/bash
# For writing patches: unpacks QEMU into DIR and applies the [qemu] patches,
# up to and including UNTIL (default: all). Needs build.sh's cache (run it once).
#   tools/qemu-src.sh DIR [UNTIL.patch]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:?usage: tools/qemu-src.sh DIR [UNTIL.patch]}
until=${2:-}
commit=$(sed -n 's/^qemu_commit=\([0-9a-f]*\).*/\1/p' "$here/build.sh")
mkdir -p "$dir"
tar -xzf "$here/.build/cache/qemu-$commit.tar.gz" -C "$dir"
src=$dir/qemu-$commit
n=0
while read -r name; do
  patch -d "$src" -p1 -f -s -i "$here/patches/$name" || { echo "$name does not apply" >&2; exit 1; }
  n=$((n + 1))
  [[ $name == "$until" ]] && break
done < <(awk '/^\[/ {on = ($1 == "[qemu]"); next} on && !/^[[:space:]]*(#|$)/ {print $1}' "$here/patches.list")
echo "$src ($n patches)"
