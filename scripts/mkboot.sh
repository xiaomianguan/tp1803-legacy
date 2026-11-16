#!/bin/bash
# Repack the ROM ramdisk with Lineage's 4.14 kernel + dtb into a v2 boot image.
# CM13's own mkbootimg can't write header v2 or a dtb section, which the
# community abl on TP1803 needs. This uses the host's modern mkbootimg.
#
# usage: scripts/mkboot.sh [ramdisk.img] [out.img] [extra cmdline...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEV="$ROOT/device/nubia/TP1803"
OUT_DIR="${OUT_DIR:-${TP_SRC:-$HOME/aospa-build/src}/out/target/product/TP1803}"
RAMDISK="${1:-$OUT_DIR/ramdisk.img}"
OUT="${2:-$OUT_DIR/boot-v2.img}"
shift 2 2>/dev/null || true
EXTRA="$*"

# Same cmdline as BOARD_KERNEL_CMDLINE (the abl appends its own androidboot.* args)
CMDLINE="androidboot.console=ttyMSM0 androidboot.hardware=qcom androidboot.usbcontroller=a600000.dwc3 console=ttyMSM0,115200n8 earlycon=msm_geni_serial,0xa90000 loop.max_part=7 lpm_levels.sleep_disabled=1 service_locator.enable=1 androidboot.selinux=disabled printk.devkmsg=on download_mode=0 $EXTRA"

mkbootimg \
    --header_version 2 \
    --os_version 6.0.1 --os_patch_level 2017-10 \
    --kernel "$DEV/prebuilt/Image.gz" \
    --ramdisk "$RAMDISK" \
    --dtb "$DEV/prebuilt/dtb" \
    --pagesize 4096 --base 0x00000000 \
    --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
    --tags_offset 0x00000100 --dtb_offset 0x01f00000 \
    --cmdline "$CMDLINE" \
    -o "$OUT"

echo "wrote $OUT ($(stat -c %s "$OUT") bytes)"
