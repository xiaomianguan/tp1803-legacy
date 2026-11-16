#!/bin/bash
# Standalone SEE test tools (NDK); the client itself lives in the sensors HAL.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../../device/nubia/TP1803/sensors"
CC="$HOME/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang"
for t in ssc-test ssc-probe ssc-dump; do
    $CC -O2 -D_GNU_SOURCE -I"$S" -o "$HERE/$t" "$HERE/$t.c" "$S/ssc.c"
done
