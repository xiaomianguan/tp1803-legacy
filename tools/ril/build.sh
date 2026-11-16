#!/bin/bash
# NDK build of RIL bring-up tools against device/nubia/TP1803/ril.
set -e
cd "$(dirname "$0")"
NDK=~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin
R=../../device/nubia/TP1803/ril
for t in "$@"; do
  $NDK/aarch64-linux-android23-clang -O2 -Wall -I$R -o $t $t.c $R/qmi.c
done
