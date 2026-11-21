#!/bin/bash
# Build a flashable MIUI 7 zip for the TP1803 from the Mi 5 (gemini) MIUI zip
# and our AOSPA build (scripts/build.sh systemimage on branch aospa-6 /
# miui-7-a6 first: the device payload comes from its out/ directory).
#
# usage: scripts/miui/port.sh <miui_MI5_...zip> [out.zip]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MIUI_ZIP="$1"
export TP_SRC=${TP_SRC:-$HOME/aospa-build/src}
WORK=${WORK:-$HOME/miui-port/work}
VER=$(basename "$MIUI_ZIP" | sed -E 's/^miui_[A-Z0-9]+_([^_]+)_.*/\1/')
OUT="${2:-$TP_SRC/out/dist-tp1803/miui-$VER-TP1803.zip}"
mkdir -p "$WORK" "$(dirname "$OUT")"

# Device file list and properties from the AOSPA product configuration
(cd "$HOME/cm13-build" && ./enter.sh bash -c 'cd /src && source build/envsetup.sh >/dev/null 2>&1 &&
    lunch pa_TP1803-userdebug >/dev/null 2>&1 &&
    echo "$(get_build_var PRODUCT_COPY_FILES)" > /src/out/miui-copyfiles.txt &&
    echo "$(get_build_var PRODUCT_PROPERTY_OVERRIDES)" > /src/out/miui-props.txt')

# Optional GApps variant: GAPPS=<open_gapps-arm64-6.0-nano-*.zip> (the same
# package as the CM/AOSPA images) -> miui-<ver>-TP1803-gapps.zip
unset MIUI_GAPPS
if [ -n "${GAPPS:-}" ]; then
    case "${2:-}" in "") OUT="$TP_SRC/out/dist-tp1803/miui-$VER-TP1803-gapps.zip";; esac
    G=$TP_SRC/out/miui-gapps
    gclean() { (cd "$HOME/cm13-build" && ./enter.sh bash -c "chmod -R u+w /src/out/miui-gapps 2>/dev/null; rm -rf /src/out/miui-gapps"); }
    gclean   # tarballs extract read-only dirs: remove from inside the chroot
    mkdir -p "$G/pkg" "$G/tars"
    unzip -q "$GAPPS" -d "$G/pkg"
    (cd "$HOME/cm13-build" && ./enter.sh bash -c 'cd /src/out/miui-gapps && for f in pkg/Core/*.tar* pkg/GApps/*.tar* pkg/Optional/*.tar*; do
        case $f in *.lz) lzip -dc "$f" | tar -x -C tars;; *) tar -x -C tars -f "$f";; esac; done')
    export MIUI_GAPPS=$G
fi
python3 -I "$ROOT/scripts/miui/port.py" "$MIUI_ZIP" "$TP_SRC" "$WORK" \
    "$TP_SRC/out/miui-copyfiles.txt" "$TP_SRC/out/miui-props.txt"

# Boot image: our kernel + dtb, the composed ramdisk
"$TP_SRC/out/host/linux-x86/bin/mkbootfs" "$WORK/ramdisk" | gzip -9 > "$WORK/ramdisk.img"
"$ROOT/scripts/mkboot.sh" "$WORK/ramdisk.img" "$WORK/boot.img"

# Flashable zip: full-image block update (transfer list v3) + boot
python3 -I - "$WORK" <<'PY'
import os, sys
w = sys.argv[1]
n = os.path.getsize(os.path.join(w, "system.img")) // 4096
open(os.path.join(w, "system.transfer.list"), "w").write(
    "3\n%d\n0\n0\nerase 2,0,%d\nnew 2,0,%d\n" % (n, n, n))
PY
rm -f "$WORK/unsigned.zip"
mkdir -p "$WORK/META-INF/com/google/android"
unzip -p "$(ls -t $TP_SRC/out/dist-tp1803/pa-6.0.3-*-TP1803.zip | head -1)" \
    META-INF/com/google/android/update-binary > "$WORK/META-INF/com/google/android/update-binary"
cat > "$WORK/META-INF/com/google/android/updater-script" <<'EDIFY'
assert(run_program("/system/bin/sh", "-c", "[ x$(getprop ro.product.device) = xTP1803 ] || [ x$(getprop ro.build.product) = xTP1803 ]") == 0 || abort("This package is for device: TP1803."););
ui_print("MIUI 7 for Nubia Mini 5G (TP1803)");
ifelse(is_mounted("/system"), unmount("/system"));
ui_print("Writing system image...");
show_progress(0.900000, 0);
block_image_update("/dev/block/bootdevice/by-name/system", package_extract_file("system.transfer.list"), "system.new.dat", "system.patch.dat");
ui_print("Writing boot image...");
package_extract_file("boot.img", "/dev/block/bootdevice/by-name/boot");
set_progress(1.000000);
EDIFY
: > "$WORK/system.patch.dat"
python3 -I - "$WORK" <<'PY'
import os, sys, zipfile
w = sys.argv[1]
with zipfile.ZipFile(os.path.join(w, "unsigned.zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as z:
    for name, src in (("META-INF/com/google/android/update-binary", None),
                      ("META-INF/com/google/android/updater-script", None),
                      ("system.transfer.list", None), ("system.new.dat", "system.img"),
                      ("system.patch.dat", None), ("boot.img", None)):
        z.write(os.path.join(w, src or name), name)
PY
mv "$WORK/unsigned.zip" "$TP_SRC/out/miui-unsigned.zip"
(cd "$HOME/cm13-build" && ./enter.sh bash -c 'cd /src && java -Xmx2g -jar out/host/linux-x86/framework/signapk.jar -w \
    build/target/product/security/testkey.x509.pem build/target/product/security/testkey.pk8 \
    out/miui-unsigned.zip out/miui-signed.zip')
mv "$TP_SRC/out/miui-signed.zip" "$OUT"
rm -f "$TP_SRC/out/miui-unsigned.zip"
ls -la "$OUT"; md5sum "$OUT"
[ -n "${MIUI_GAPPS:-}" ] && gclean || true
