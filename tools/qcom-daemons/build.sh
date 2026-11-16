#!/bin/bash
# Build linux-msm qrtr-ns / pd-mapper / rmtfs / tqftpserv + modem-holder as
# static aarch64 executables for Android 6 (API 23).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
R=~/cm13-build/radio
CC="$HOME/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang"
OUT="$HERE/out"; mkdir -p "$OUT"
CFLAGS="-O2 -DANDROID -D_GNU_SOURCE -Wno-unused-result -Wno-format"
LIBQRTR="$R/qrtr-ns/lib/qrtr.c $R/qrtr-ns/lib/qmi.c $R/qrtr-ns/lib/logging.c"
QINC="-I$R/qrtr-ns/include -I$R/qrtr-ns/lib"

$CC $CFLAGS $QINC -I$R/qrtr-ns/src -o $OUT/qrtr-ns \
    $R/qrtr-ns/src/ns.c $R/qrtr-ns/src/addr.c $R/qrtr-ns/src/map.c $R/qrtr-ns/src/hash.c \
    $R/qrtr-ns/src/waiter.c $R/qrtr-ns/src/util.c $LIBQRTR
# pd-mapper's upstream ANDROID branch doesn't build; its Linux path + our
# firmware-dir fallback is what we want anyway.
$CC ${CFLAGS/-DANDROID/} $QINC -I$HERE/stubinc -o $OUT/pd-mapper \
    $R/pd-mapper/pd-mapper.c $R/pd-mapper/assoc.c $R/pd-mapper/json.c \
    $R/pd-mapper/servreg_loc.c $HERE/lzma_stub.c $LIBQRTR
$CC $CFLAGS $QINC -o $OUT/rmtfs \
    $R/rmtfs/rmtfs.c $R/rmtfs/qmi_rmtfs.c $R/rmtfs/sharedmem.c $R/rmtfs/storage.c \
    $R/rmtfs/util.c $R/rmtfs/rproc.c $LIBQRTR
$CC $CFLAGS $QINC -o $OUT/tqftpserv \
    $R/tqftpserv/tqftpserv.c $R/tqftpserv/translate.c $LIBQRTR
$CC ${CFLAGS/-static/} -o $OUT/modem-holder $HERE/modem-holder.c -llog

$CC ${CFLAGS/-static/} -o $OUT/mdm-boot $HERE/mdm-boot.c -llog
$CC ${CFLAGS/-static/} -o $OUT/mdm-ctl $HERE/mdm-ctl.c
$CC $CFLAGS $QINC -o $OUT/wlfw-bdf $HERE/wlfw-bdf.c $LIBQRTR -llog
ls -la $OUT
