#!/bin/bash
# Build the libnativewindow shim for both ABIs with the NDK against M headers.
set -euo pipefail
cd "$(dirname "$0")"
NDK=~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin
M=~/cm13-build/src
INC="-I$M/system/core/include -I$M/hardware/libhardware/include"
mkdir -p out/lib64 out/lib
$NDK/aarch64-linux-android23-clang -O2 -fPIC -shared -fvisibility=hidden $INC \
  -Wl,-soname,libnativewindow.so -o out/lib64/libnativewindow.so nativewindow_shim.c
$NDK/armv7a-linux-androideabi23-clang -O2 -fPIC -shared -fvisibility=hidden $INC \
  -Wl,-soname,libnativewindow.so -o out/lib/libnativewindow.so nativewindow_shim.c
ls -la out/lib64 out/lib
