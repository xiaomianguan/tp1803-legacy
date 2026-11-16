#!/bin/bash
# Camera bring-up test programs (NDK, against CM13's hardware headers).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S=~/cm13-build/src
CC="$HOME/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang"
INC="-I$S/hardware/libhardware/include -I$S/system/core/include -I$S/system/media/camera/include"
for t in "$@"; do
    $CC -O2 $INC -o "$HERE/$t" "$HERE/$t.c" -ldl ${LIBS:-}
done
