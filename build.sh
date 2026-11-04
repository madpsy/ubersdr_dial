#!/usr/bin/env bash
# build.sh - build the firmware and stage the binaries the browser flasher
# (docs/flash/, published by GitHub Pages from /docs) needs.
#
# Usage: ./build.sh
#
# Then commit docs/flash/*.bin. Wi-Fi settings are never part of these: the
# flasher page adds them in the user's browser (see docs/flash/index.html).
set -euo pipefail
cd "$(dirname "$0")"

ENV="jc3636k718"
BUILD_DIR=".pio/build/$ENV"
OUT="docs/flash"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

step() { echo; echo "▶ $*"; }
ok()   { echo "✓ $*"; }
fail() { echo "✗ $*" >&2; exit 1; }

step "Building firmware..."
pio run -e "$ENV" || fail "Build failed"
ok "Build complete"

step "Staging web-flasher binaries in $OUT/ ..."
[[ -f "$BOOT_APP0" ]] || fail "boot_app0.bin not found at $BOOT_APP0"
mkdir -p "$OUT"
cp "$BUILD_DIR/bootloader.bin" "$OUT/bootloader.bin"
cp "$BUILD_DIR/partitions.bin" "$OUT/partitions.bin"
cp "$BOOT_APP0"                "$OUT/boot_app0.bin"
cp "$BUILD_DIR/firmware.bin"   "$OUT/firmware.bin"
ok "Binaries staged"

echo
echo "✓ Done. Commit docs/flash/*.bin to publish them via the web flasher."
