#!/bin/bash
# Test client for the sensors HAL; built with the NDK against CM's hardware headers.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
S=~/cm13-build/src
CC="$HOME/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android23-clang"
$CC -O2 -I$S/hardware/libhardware/include -I$S/system/core/include -o "$HERE/sensors-test" "$HERE/sensors-test.c" -ldl
