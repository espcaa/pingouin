#!/bin/bash
# Builds Pingouin.app into dist/: the SwiftPM app, its Info.plist, and the QEMU
# runtime from runtime/.build (when it's been built), signed.
#   SIGN_ID="Developer ID Application: ..." scripts/bundle.sh   (default: the dev certificate, or ad-hoc)
set -euo pipefail
app_dir=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$app_dir/.." && pwd)
out=$root/dist/Pingouin.app
# SIGN_ID, else the dev certificate scripts/make-dev-cert.sh made (its name in
# git config pingouin.devcert; macOS keeps the permissions across rebuilds),
# else ad-hoc (permissions lost on every rebuild).
dev_cert=$(git -C "$root" config pingouin.devcert || true)
if [[ -n ${SIGN_ID:-} ]]; then
  sign_id=$SIGN_ID
elif [[ -n $dev_cert ]] && security find-identity -v -p codesigning 2>/dev/null | grep -qF "\"$dev_cert\""; then
  sign_id=$dev_cert
else
  sign_id=-
  echo "bundle: signing ad-hoc; run scripts/make-dev-cert.sh once to keep permissions across rebuilds" >&2
fi
bundle_id=${BUNDLE_ID:-io.github.espcaa.pingouin}
version=${VERSION:-$(cat "$root/VERSION")}
# RELEASE=1 (scripts/release.sh): the update feed goes in, so the app checks
# GitHub for new versions. Dev builds have none: they never "update" over dist/.
release=${RELEASE:-0}
feed_url=${FEED_URL:-https://github.com/espcaa/pingouin/releases/latest/download/appcast.xml}
# Sparkle's public key (its private half: the login keychain, scripts/release.sh, CI secrets)
sparkle_public_key=jIppggsGP5QV4Af83Mj2ILCPclS8nSi1qXzhDjEgWuw=

cd "$app_dir"
swift build -c release --product Pingouin
bin=$(swift build -c release --show-bin-path)/Pingouin

rm -rf "$out"
mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"
cp "$bin" "$out/Contents/MacOS/Pingouin"
sed -e "s/@BUNDLE_ID@/$bundle_id/" -e "s/@VERSION@/$version/" Resources/Info.plist > "$out/Contents/Info.plist"
if [[ $release == 1 ]]; then
  pb=/usr/libexec/PlistBuddy
  $pb -c "Add :SUFeedURL string $feed_url" "$out/Contents/Info.plist"
  $pb -c "Add :SUPublicEDKey string $sparkle_public_key" "$out/Contents/Info.plist"
  $pb -c "Add :SUEnableAutomaticChecks bool true" "$out/Contents/Info.plist"
fi
# Sparkle (updates)
mkdir -p "$out/Contents/Frameworks"
ditto "$(swift build -c release --show-bin-path)/Sparkle.framework" "$out/Contents/Frameworks/Sparkle.framework"
[[ -f Resources/Pingouin.icns ]] && cp Resources/Pingouin.icns "$out/Contents/Resources/"
# the VM windows' Dock icon (QEMU sets it while running: flat only)
[[ -f Resources/VM.icns ]] && cp Resources/VM.icns "$out/Contents/Resources/"
# macOS 26 and later: the Liquid Glass icon, compiled from Resources/Pingouin.icon
# (Icon Composer). That needs actool from Xcode 26+ (GitHub's macOS runners have
# it); without it, and on older macOS, the app shows Pingouin.icns (flat).
if [[ -d Resources/Pingouin.icon ]] && xcrun --find actool >/dev/null 2>&1; then
  icon_tmp=$(mktemp -d)
  if xcrun actool Resources/Pingouin.icon --compile "$out/Contents/Resources" \
      --app-icon Pingouin --platform macosx --minimum-deployment-target 15.0 \
      --output-partial-info-plist "$icon_tmp/icon.plist" >"$icon_tmp/actool.log" 2>&1; then
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconName string Pingouin" "$out/Contents/Info.plist" 2>/dev/null ||
      /usr/libexec/PlistBuddy -c "Set :CFBundleIconName Pingouin" "$out/Contents/Info.plist"
    echo "bundle: Liquid Glass icon compiled (actool)" >&2
  else
    echo "bundle: actool failed, flat icon only:" >&2; tail -5 "$icon_tmp/actool.log" >&2
  fi
  rm -rf "$icon_tmp"
fi

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

# Sparkle's helpers (Autoupdate, Updater.app, its XPC services) and the framework
codesign --force --deep ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" "$out/Contents/Frameworks/Sparkle.framework"
codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" "$out/Contents/MacOS/Pingouin"
codesign --force ${hardened[@]+"${hardened[@]}"} --sign "$sign_id" "$out"
codesign --verify --strict "$out" && echo "$out"
