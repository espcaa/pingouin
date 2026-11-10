#!/bin/bash
# Makes a self-signed code-signing certificate in the login keychain, once per
# Mac:  scripts/make-dev-cert.sh "Your Name"   (default: "Pingouin Dev").
# Its name goes in this repo's local git config (pingouin.devcert, never
# committed); bundle.sh signs with it when it's there, so every
# build has the same identity and macOS keeps its Accessibility and Input
# Monitoring permissions across rebuilds (ad-hoc builds lose them).
# Not for distribution: other Macs don't trust it (that needs a Developer ID).
set -euo pipefail
name=${1:-Pingouin Dev}
keychain=$HOME/Library/Keychains/login.keychain-db

if security find-identity -v -p codesigning | grep -qF "\"$name\""; then
  git config pingouin.devcert "$name"
  echo "\"$name\" is already set up"; exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/cert.conf" <<CONF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $name
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CONF
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -config "$tmp/cert.conf" -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null
pass=$(/usr/bin/openssl rand -hex 16)
/usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" \
  -name "$name" -passout "pass:$pass" -out "$tmp/cert.p12"
# -T: codesign may use the key without asking each time
security import "$tmp/cert.p12" -k "$keychain" -P "$pass" -T /usr/bin/codesign >/dev/null
echo "Trusting \"$name\" for code signing (macOS asks for your password)..."
security add-trusted-cert -p codeSign -k "$keychain" "$tmp/cert.pem"
security find-identity -v -p codesigning | grep -F "\"$name\""
git config pingouin.devcert "$name"   # for bundle.sh
