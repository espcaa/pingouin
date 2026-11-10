#!/bin/bash
# Builds pingouind for the VM (linux/arm64, static) into dist/guest/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/dist/guest
mkdir -p "$out/pingouind"
cd "$root/guest/pingouind"
go test ./...
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath \
  -ldflags "-s -w -X main.version=${VERSION:-dev}" -o "$out/pingouind/pingouind" .
cp pingouind.service pingouin-displays.service pingouin-displays.desktop pingouind.openrc pingouind.runit "$out/pingouind/"
cp "$root/guest/install.sh" "$out/"
rm -rf "$out/nixos" && cp -R "$root/guest/nixos" "$out/nixos"
echo "$out"
