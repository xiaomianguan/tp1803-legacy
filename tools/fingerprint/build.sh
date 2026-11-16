#!/bin/bash
# NDK build of the fingerprint HAL test tool.
set -e
cd "$(dirname "$0")"
NDK=~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin
$NDK/aarch64-linux-android23-clang -O2 -Wall -include sys/stat.h -o fp-test fp-test.c
