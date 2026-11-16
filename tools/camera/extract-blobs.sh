#!/bin/bash
# Copy the stock CamX camera HAL and everything it needs (as resolved by
# abi-gap.py against CM13's /system) from the Lineage vendor image into the
# device tree. libqdMetaData/libcdsprpc are replaced by our stubs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
V=${1:-$HOME/cm13-build/vendor-lineage}
OUT="$HERE/../../device/nubia/TP1803/proprietary/camera"
mkdir -p "$OUT"   # files are overwritten in place
python3 -I "$HERE/abi-gap.py" "$V" "$HOME/cm13-build/src/out/target/product/TP1803/system" > "$HERE/abi-gap.txt"
grep '^  lib64/' "$HERE/abi-gap.txt" | while read -r f; do
    mkdir -p "$OUT/$(dirname "$f")"; cp "$V/$f" "$OUT/$f"
done
# tuning/sensor module data next to the sensor libraries, config, ICP firmware
cp "$V"/lib64/camera/*.bin "$OUT/lib64/camera/"
mkdir -p "$OUT/etc/camera" "$OUT/firmware"
cp "$V"/etc/camera/* "$OUT/etc/camera/"
# CamX settings: outputFormat=0 makes preview/video (IMPLEMENTATION_DEFINED)
# linear NV12 instead of UBWC-compressed, which nothing on CM13 can read.
cat > "$OUT/etc/camera/camxoverridesettings.txt" <<'SETTINGS'
enableNCSService=FALSE
outputFormat=0
SETTINGS
cp "$V/firmware/CAMERA_ICP.elf" "$OUT/firmware/"
find "$OUT" -type f | wc -l
du -sh "$OUT"
