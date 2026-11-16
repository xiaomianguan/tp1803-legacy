#!/bin/bash
# Build a recovery-flashable CM13 zip for TP1803 from the latest target-files
# package (make target-files-package) and our boot-v2.img (scripts/mkboot.sh).
#
#  - boot: CM13's own boot.img can't boot here, so boot-v2.img replaces it
#    in the target files before ota_from_target_files runs.
#  - device check: CM13's static updater reads properties the Android 6 way,
#    which returns "" under the Android 10 based recovery, so the generated
#    getprop() assert always fails. It is replaced by a check that runs the
#    recovery's own getprop.
#
# usage: [TF=target_files.zip] scripts/mkota.sh [out.zip]
#   TF defaults to the newest target-files package of the build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export TP_SRC=${TP_SRC:-$HOME/aospa-build/src}
SRC=$TP_SRC
PRODUCT=$SRC/out/target/product/TP1803
DIST=$SRC/out/dist-tp1803
OUT="${1:-$DIST/pa-6.0.3-$(date +%Y%m%d)-TP1803.zip}"
mkdir -p "$DIST"

TF=${TF:-$(ls -t "$PRODUCT"/obj/PACKAGING/target_files_intermediates/*.zip | head -1)}
echo "target files: $TF"
[ -s "$PRODUCT/boot-v2.img" ] || { echo "no boot-v2.img: run scripts/mkboot.sh"; exit 1; }

python3 -I - "$TF" "$DIST/target_files-bootv2.zip" "$PRODUCT/boot-v2.img" <<'EOF'
import sys, zipfile, shutil
src, dst, boot = sys.argv[1:]
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as zo:
    for info in zi.infolist():
        if info.filename in ("BOOTABLE_IMAGES/boot.img", "IMAGES/boot.img"):
            continue
        with zi.open(info) as f, zo.open(info, 'w', force_zip64=True) as g:
            shutil.copyfileobj(f, g, 1 << 20)
    for name in ("BOOTABLE_IMAGES/boot.img", "IMAGES/boot.img"):
        zo.write(boot, name)
EOF

cd "$HOME/cm13-build"
./enter.sh bash -c 'cd /src && source build/envsetup.sh >/dev/null && lunch pa_TP1803-userdebug >/dev/null 2>&1 &&
  ./build/tools/releasetools/ota_from_target_files --block \
    -p out/host/linux-x86 -k build/target/product/security/testkey \
    out/dist-tp1803/target_files-bootv2.zip out/dist-tp1803/ota-unfixed.zip' | tail -3

# Swap the device assert, then re-sign the whole file.
python3 -I - "$DIST/ota-unfixed.zip" "$DIST/ota-unsigned.zip" <<'EOF'
import sys, zipfile, shutil
src, dst = sys.argv[1:]
SCRIPT = "META-INF/com/google/android/updater-script"
CHECK = ('assert(run_program("/system/bin/sh", "-c", '
         '"[ x$(getprop ro.product.device) = xTP1803 ] || [ x$(getprop ro.build.product) = xTP1803 ]") == 0 '
         '|| abort("This package is for device: TP1803."););')
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as zo:
    for info in zi.infolist():
        if info.filename.startswith("META-INF/") and info.filename.split("/")[-1] in ("MANIFEST.MF", "CERT.SF", "CERT.RSA"):
            continue
        if info.filename == SCRIPT:
            lines = zi.read(info).decode().split("\n")
            out, swapped = [], False
            for l in lines:
                # Device check: CM's getprop() assert or AOSPA's
                # get_device_compatible() -> recovery-side getprop check.
                if l.startswith('assert(getprop("ro.product.device")') or \
                        l.startswith('get_device_compatible('):
                    out.append(CHECK)
                    swapped = True
                # AOSPA's downgrade check compares getprop("ro.build.date.utc"),
                # which the updater can't read here either: drop it.
                elif 'getprop("ro.build.date.utc")' in l:
                    continue
                else:
                    out.append(l)
            assert swapped, "no device check found in updater-script"
            zo.writestr(info, "\n".join(out))
            continue
        with zi.open(info) as f, zo.open(info, 'w', force_zip64=True) as g:
            shutil.copyfileobj(f, g, 1 << 20)
EOF

./enter.sh bash -c 'cd /src && java -Xmx2g -jar out/host/linux-x86/framework/signapk.jar -w \
    build/target/product/security/testkey.x509.pem build/target/product/security/testkey.pk8 \
    out/dist-tp1803/ota-unsigned.zip out/dist-tp1803/ota-signed.zip'
mv "$DIST/ota-signed.zip" "$OUT"
rm -f "$DIST/ota-unfixed.zip" "$DIST/ota-unsigned.zip"
unzip -p "$OUT" META-INF/com/google/android/updater-script | head -1
ls -la "$OUT"
md5sum "$OUT"
