#!/bin/bash
# Builds pingouind for the VM (linux/arm64, static) into dist/guest/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/dist/guest
mkdir -p "$out/pingouind"
# The tools' version: a hash of their sources, unless VERSION is given. pingouind
# reports it to the app, which compares it with dist/guest/VERSION (outdated?).
version=${VERSION:-$(cd "$root/guest" && find . -type f ! -path './pingouind/pingouind' ! -name '.DS_Store' \
  -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-12)}
cd "$root/guest/pingouind"
go test ./...
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath \
  -ldflags "-s -w -X main.version=$version" -o "$out/pingouind/pingouind" .
echo "$version" > "$out/VERSION"
cp pingouind.service pingouin-displays.service pingouin-displays.desktop pingouind.openrc pingouind.runit "$out/pingouind/"
cp "$root/guest/install.sh" "$out/"
rm -rf "$out/nixos" && cp -R "$root/guest/nixos" "$out/nixos"
echo "$out"
