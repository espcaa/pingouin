#!/bin/bash
# Builds a release into dist/release/: the runtime, the guest tools,
# Pingouin.app (signed, with the update feed), Pingouin-<version>.dmg and
# appcast.xml (Sparkle's feed, for the GitHub release).
#   app/scripts/release.sh                      (version: ./VERSION, signed as Alice)
#   VERSION=1.0.1 SIGN_ID="Alice" app/scripts/release.sh
#   SKIP_RUNTIME=1: reuse runtime/.build (already built)
# Sparkle's private key: the login keychain (generate_keys), or the file at
# SPARKLE_KEY_FILE (GitHub Actions).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
version=${VERSION:-$(cat "$root/VERSION")}
sign_id=${SIGN_ID:-Alice}
repo=${REPO:-espcaa/pingouin}
out=$root/dist/release
sparkle_bin=$root/app/.build/artifacts/sparkle/Sparkle/bin

[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "release: version '$version' isn't x.y.z" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"

if [[ -z ${SKIP_RUNTIME:-} ]]; then
  "$root/runtime/build.sh"
fi
(cd "$root/app" && swift package resolve >/dev/null)
RELEASE=1 VERSION=$version SIGN_ID=$sign_id "$root/app/scripts/bundle.sh"

# The DMG: the app and a shortcut to /Applications
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
ditto "$root/dist/Pingouin.app" "$stage/Pingouin.app"
ln -s /Applications "$stage/Applications"
dmg=$out/Pingouin-$version.dmg
hdiutil create -volname "Pingouin $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg" >/dev/null
codesign --force --sign "$sign_id" "$dmg"

# Sparkle: the DMG's EdDSA signature, then a feed with this version
key_args=()
[[ -n ${SPARKLE_KEY_FILE:-} ]] && key_args=(--ed-key-file "$SPARKLE_KEY_FILE")
sig=$("$sparkle_bin/sign_update" ${key_args[@]+"${key_args[@]}"} "$dmg")   # sparkle:edSignature="…" length="…"
[[ $sig == *edSignature* ]] || { echo "release: sign_update failed: $sig" >&2; exit 1; }
cat > "$out/appcast.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Pingouin</title>
    <link>https://github.com/$repo</link>
    <item>
      <title>Pingouin $version</title>
      <pubDate>$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')</pubDate>
      <sparkle:version>$version</sparkle:version>
      <sparkle:shortVersionString>$version</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>15.0</sparkle:minimumSystemVersion>
      <sparkle:releaseNotesLink>https://github.com/$repo/releases/tag/v$version</sparkle:releaseNotesLink>
      <enclosure url="https://github.com/$repo/releases/download/v$version/Pingouin-$version.dmg" $sig type="application/octet-stream"/>
    </item>
  </channel>
</rss>
XML

echo "release: $out"
ls -la "$out"
