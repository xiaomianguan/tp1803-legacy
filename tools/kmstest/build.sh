#!/bin/sh
# Freestanding static aarch64 build with host clang + lld (no sysroot needed).
cd "$(dirname "$0")"
clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -nostdlib -static -fno-stack-protector \
  -I/usr/include -fuse-ld=lld -Wl,-e,_start -o kmstest kmstest.c
