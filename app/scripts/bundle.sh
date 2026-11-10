#!/bin/bash
# Builds Pingouin.app into dist/: the SwiftPM app, its Info.plist, and the QEMU
# runtime from runtime/.build (when it's been built), signed.
#   SIGN_ID="Developer ID Application: ..." scripts/bundle.sh   (default: ad-hoc)
set -euo pipefail
app_dir=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$app_dir/.." && pwd)
out=$root/dist/Pingouin.app
sign_id=${SIGN_ID:--}
bundle_id=${BUNDLE_ID:-app.pingouin.Pingouin}
version=${VERSION:-0.1.0}

cd "$app_dir"
swift build -c release --product Pingouin
bin=$(swift build -c release --show-bin-path)/Pingouin

rm -rf "$out"
mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"
cp "$bin" "$out/Contents/MacOS/Pingouin"
sed -e "s/@BUNDLE_ID@/$bundle_id/" -e "s/@VERSION@/$version/" Resources/Info.plist > "$out/Contents/Info.plist"
[[ -f Resources/Pingouin.icns ]] && cp Resources/Pingouin.icns "$out/Contents/Resources/"

runtime=$root/runtime/.build/qemu-gpu-runtime
if [[ -d $runtime ]]; then
  ditto "$runtime" "$out/Contents/Resources/runtime"
else
  echo "bundle: build the runtime first: runtime/build.sh" >&2
  exit 1
fi

# The guest tools (pingouind, install.sh), shown to every VM read-only
"$root/guest/build.sh" >/dev/null
ditto "$root/dist/guest" "$out/Contents/Resources/guest"

# The hardened runtime turns on library validation: QEMU may then only load
# dylibs signed by the same team. Ad-hoc and self-signed (development)
# signatures have no team, so only a Developer ID gets the hardened runtime
# (notarization needs it); every dylib is signed with the same identity.
if [[ $sign_id == "Developer ID Application:"* ]]; then
  hardened=(--options runtime)
else
  hardened=()
fi
res=$out/Contents/Resources/runtime
find "$res/lib" -name '*.dylib' -type f -print0 |
  xargs -0 codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id"
# QEMU needs the hypervisor entitlement
codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" \
  --entitlements "$root/runtime/qemu-hvf.entitlements" "$res/bin/qemu-system-aarch64"

codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" "$out/Contents/MacOS/Pingouin"
codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" "$out"
codesign --verify --strict "$out" && echo "$out"
