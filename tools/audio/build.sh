#!/bin/bash
# NDK build of audio bring-up tools (tinyalsa from the CM tree).
set -e
cd "$(dirname "$0")"
NDK=~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin
TA=~/cm13-build/src/external/tinyalsa
for t in "$@"; do
  $NDK/aarch64-linux-android23-clang -O2 -w -I$TA/include -o $t $t.c $TA/pcm.c $TA/mixer.c
done
