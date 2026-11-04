#!/usr/bin/env bash
# release.sh - cut a release: version everything, rebuild the web flasher's
# binaries, commit, tag and push. GitHub Pages then serves the new flasher.
#
# Usage: ./release.sh 1.2.0
#
# The version lives in VERSION. It is compiled into the firmware (version.py ->
# DIAL_VERSION, shown on the start screen) and written into the flasher's
# manifest and page here, so all three always agree.
set -euo pipefail
cd "$(dirname "$0")"

step() { echo; echo "▶ $*"; }
ok()   { echo "✓ $*"; }
fail() { echo "✗ $*" >&2; exit 1; }

VER="${1:-}"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Usage: ./release.sh X.Y.Z (now: $(cat VERSION))"
git rev-parse -q --verify "refs/tags/v$VER" >/dev/null && fail "Tag v$VER already exists"

step "Checking the repo..."
[[ "$(git rev-parse --abbrev-ref HEAD)" == "main" ]] || fail "Not on main"
[[ -z "$(git status --porcelain)" ]] || { git status --short; fail "Uncommitted changes: commit or stash them first"; }
git fetch -q origin
[[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main)" ]] || fail "main isn't level with origin/main: pull or push first"
ok "Clean and up to date"

step "Setting version $VER..."
echo "$VER" > VERSION
sed -i -E "s/(\"version\": *\")[^\"]*\"/\1$VER\"/" docs/flash/manifest.json
sed -i -E "s/(var VERSION = ')[^']*'/\1$VER'/" docs/flash/index.html
grep -q "\"version\": \"$VER\"" docs/flash/manifest.json || fail "manifest.json version not set"
grep -q "var VERSION = '$VER'" docs/flash/index.html || fail "index.html version not set"
ok "VERSION, manifest.json and index.html say $VER"

step "Building and staging the flasher binaries..."
./build.sh
for f in bootloader partitions firmware; do
  cmp -s ".pio/build/jc3636k718/$f.bin" "docs/flash/$f.bin" || fail "docs/flash/$f.bin doesn't match the build"
done
strings docs/flash/firmware.bin | grep -q "v$VER" || fail "firmware.bin doesn't contain v$VER"
ok "Binaries are this build, and the firmware says v$VER"

step "Committing, tagging and pushing..."
git add VERSION docs/flash
git commit -q -m "Release v$VER"
git tag -a "v$VER" -m "UberSDR Dial v$VER"
git push -q origin main "v$VER"
ok "Pushed main and v$VER"

echo
echo "✓ Released v$VER. The web flasher updates once GitHub Pages redeploys (a minute or two):"
echo "  https://madpsy.github.io/ubersdr_dial/flash/"
