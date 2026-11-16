#!/bin/bash
# Build Mesa freedreno (Gallium, KGSL backend) for Android 6 (API 23) with the NDK.
# usage: build-mesa.sh [aarch64|arm]
set -euo pipefail
ARCH="${1:-aarch64}"
GPU=~/cm13-build/gpu
NDK=$GPU/android-ndk-r27d
SRC=$GPU/mesa-kgsl
BUILD=$GPU/build-$ARCH
API=23
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
case $ARCH in
  aarch64) TRIPLE=aarch64-linux-android; CPU=aarch64; FAM=aarch64 ;;
  arm)     TRIPLE=armv7a-linux-androideabi; CPU=armv7; FAM=arm ;;
esac
export PATH=$GPU/venv/bin:$PATH
mkdir -p $BUILD
cat > $BUILD/cross.ini <<INI
[binaries]
ar = '$TC/llvm-ar'
c = ['ccache', '$TC/${TRIPLE}${API}-clang']
cpp = ['ccache', '$TC/${TRIPLE}${API}-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$GPU/deps-$ARCH/lib/pkgconfig', 'PKG_CONFIG_SYSROOT_DIR=', '/usr/bin/pkg-config', '--static']

[host_machine]
system = 'android'
cpu_family = '$FAM'
cpu = '$CPU'
endian = 'little'
INI
# Static libdrm for the target (headers + xf86drm; no device-specific libs)
DEPS=$GPU/deps-$ARCH
if [ ! -f $DEPS/lib/pkgconfig/libdrm.pc ]; then
  meson setup $GPU/libdrm-build-$ARCH $GPU/libdrm --cross-file $BUILD/cross.ini \
    --prefix=$DEPS --libdir=lib -Ddefault_library=static -Dbuildtype=release \
    -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
    -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
    -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled -Dcairo-tests=disabled \
    -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false
  ninja -C $GPU/libdrm-build-$ARCH install
fi
if [ ! -f $BUILD/build.ninja ]; then
  meson setup $BUILD $SRC --cross-file $BUILD/cross.ini \
    -Dbuildtype=release -Db_ndebug=true \
    -Dplatforms=android -Dplatform-sdk-version=25 -Dandroid-stub=true \
    -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=kgsl \
    -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
    -Dglx=disabled -Dgbm=disabled -Dshared-glapi=enabled \
    -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
    -Dexpat=disabled -Dperfetto=false -Dandroid-libbacktrace=disabled \
    -Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa \
    -Dcpp_rtti=false -Ddri-drivers-path=$( [ $ARCH = aarch64 ] && echo /system/lib64/dri || echo /system/lib/dri ) \
    -Dc_args="-include $(dirname $(readlink -f $0))/android23_compat.h" \
    -Dcpp_args="-include $(dirname $(readlink -f $0))/android23_compat.h"
fi
ninja -C $BUILD
