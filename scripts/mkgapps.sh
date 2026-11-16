#!/bin/bash
# Build the GApps variant zip: the vanilla build's target files + OpenGApps
# 6.0 arm64 nano baked into /system (scripts/add-gapps.py), packaged by
# scripts/mkota.sh as cm-13.0-YYYYMMDD-TP1803-gapps.zip.
#
# usage: scripts/mkgapps.sh [open_gapps-arm64-6.0-nano-*.zip]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC=$HOME/cm13-build/src
PRODUCT=$SRC/out/target/product/TP1803
DIST=$SRC/out/dist-tp1803
GAPPS="${1:-$(ls -t $HOME/cm13-build/gapps/open_gapps-arm64-6.0-nano-*.zip | head -1)}"
W=$SRC/out/gapps-work
echo "gapps: $GAPPS"

clean() { (cd "$HOME/cm13-build" && ./enter.sh bash -c "chmod -R u+w /src/out/gapps-work 2>/dev/null; rm -rf /src/out/gapps-work"); }
clean  # tarballs extract read-only dirs; remove from inside the chroot
mkdir -p "$W/pkg" "$W/tars"
unzip -q "$GAPPS" -d "$W/pkg"
# The packages are lzip tarballs; lzip lives in the build chroot.
cd "$HOME/cm13-build"
./enter.sh bash -c 'cd /src/out/gapps-work && for f in pkg/Core/*.tar* pkg/GApps/*.tar* pkg/Optional/*.tar*; do
    case $f in *.lz) lzip -dc "$f" | tar -x -C tars;; *) tar -x -C tars -f "$f";; esac; done'

TF=$(ls -t "$PRODUCT"/obj/PACKAGING/target_files_intermediates/*.zip | head -1)
python3 -I "$ROOT/scripts/add-gapps.py" "$TF" "$W/tars" "$W/pkg/g.prop" "$DIST/target_files-gapps.zip"
TF="$DIST/target_files-gapps.zip" "$ROOT/scripts/mkota.sh" "$DIST/cm-13.0-$(date +%Y%m%d)-TP1803-gapps.zip"
clean; rm -f "$DIST/target_files-gapps.zip"
