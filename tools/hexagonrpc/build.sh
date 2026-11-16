#!/bin/bash
# Build hexagonrpcd (FastRPC reverse tunnel: serves the sensor registry/config
# to the SLPI) for Android 6 on the downstream adsprpc driver.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
H=~/cm13-build/radio/hexagonrpc
CC="$HOME/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang"
OUT="$HERE/out"; mkdir -p "$OUT"
D=$H/hexagonrpcd
$CC -O2 -U_FORTIFY_SOURCE -D_GNU_SOURCE -Wno-format -I$H/include -I$D \
    ${EXTRA:-} -o $OUT/${NAME:-hexagonrpcd} \
    $D/apps_mem.c $D/apps_std.c $D/interface/adsp_default_listener.c $D/interface/adsp_listener.c \
    $D/interface/apps_mem.c $D/interface/apps_std.c $D/hexagonfs.c $D/hexagonfs_mapped.c \
    $D/hexagonfs_plat_subtype_name.c $D/hexagonfs_virt_dir.c $D/iobuffer.c $D/listener.c \
    $D/localctl.c $D/rpcd.c $D/rpcd_builder.c \
    $H/libhexagonrpc/error.c $H/libhexagonrpc/handle.c $H/libhexagonrpc/hexagonrpc.c \
    $H/libhexagonrpc/interface/remotectl.c $H/libhexagonrpc/session.c \
    $HERE/fastrpc_compat.c -Wl,--wrap=open -Wl,--wrap=ioctl
ls -la $OUT
