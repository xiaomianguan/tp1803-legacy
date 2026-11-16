#!/bin/bash
# NDK build of the Bluetooth bring-up test tool.
set -e
cd "$(dirname "$0")"
NDK=~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin
B=../../device/nubia/TP1803/bluetooth
$NDK/aarch64-linux-android23-clang -O2 -Wall -I$B -o bt-init-test bt-init-test.c $B/qca_wcn399x.c
