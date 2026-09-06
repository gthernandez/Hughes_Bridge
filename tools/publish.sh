#!/usr/bin/env bash
# publish.sh -- hash the built factory OTA image, write the manifest, scp both to Dreamhost.
#
# The OTA artifact MUST be the `-e factory` build (the `-e bridge` build bakes in include/config.h ->
# Wi-Fi password + Watchdog MAC, extractable with `strings`). Build it FIRST in the Windows shell (pio
# refuses to run under git-bash on this box):
#     PYTHONIOENCODING=utf-8 pio run -e factory
# then run this from git-bash:  tools/publish.sh [manifest.json]   (default hughes.json)
#
# The manifest version is derived from FW_VERSION in src/main.cpp -- NOT hand-typed -- so it always
# matches the bin it points at (a mismatch makes every device re-offer the "update" forever). Bump
# FW_VERSION + rebuild before publishing a new release.
set -euo pipefail
cd "$(dirname "$0")/.."

MANIFEST="${1:-hughes.json}"
HOST="youruser@firmware.flensor.com"     # backup: youruser@flensor.com until DNS fully propagates
WEBDIR="firmware.flensor.com"              # ~/firmware.flensor.com on Dreamhost (the domain's web root)
BASEURL="https://firmware.flensor.com"
BIN=".pio/build/factory/firmware.bin"

VER=$(grep -oE '#define FW_VERSION "[^"]+"' src/main.cpp | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
[ -n "$VER" ] || { echo "ERROR: could not read FW_VERSION from src/main.cpp"; exit 1; }
[ -f "$BIN" ] || { echo "ERROR: $BIN missing -- run 'pio run -e factory' in the Windows shell first"; exit 1; }

# Guard: never publish a bridge build (creds baked in). The factory build must have NO Wi-Fi password.
if strings "$BIN" | grep -q "PUT-YOUR-WIFI-PASSWORD-HERE"; then
  echo "ERROR: $BIN contains the Wi-Fi password -- that's a -e bridge build, NOT -e factory. Aborting."; exit 1
fi

SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
SIZE=$(stat -c%s "$BIN" 2>/dev/null || stat -f%z "$BIN")
BINNAME="hughes-$VER.bin"

printf '{"version":"%s","url":"%s/%s","sha256":"%s","size":%s,"notes":"hughes_bridge %s"}\n' \
  "$VER" "$BASEURL" "$BINNAME" "$SHA" "$SIZE" "$VER" > "/tmp/$MANIFEST"

echo "publishing v$VER  ($SIZE bytes, sha ${SHA:0:12}...)  as $BINNAME + $MANIFEST"
scp "$BIN" "$HOST:$WEBDIR/$BINNAME"
scp "/tmp/$MANIFEST" "$HOST:$WEBDIR/$MANIFEST"
echo "done -> $BASEURL/$MANIFEST  (device manifest URL: set otaurl to this)"
