#!/bin/bash
# Copy the built Mesa (both ABIs) and Adreno firmware into the device tree.
set -euo pipefail
G=~/cm13-build/gpu
D="$(cd "$(dirname "$0")/../.." && pwd)/device/nubia/TP1803"
for a in aarch64:lib64 arm:lib; do
  B=$G/build-${a%%:*}; L=${a##*:}
  mkdir -p $D/prebuilt/mesa/$L/egl $D/prebuilt/mesa/$L/dri
  cp $B/src/egl/libEGL_mesa.so $B/src/mapi/es1api/libGLESv1_CM_mesa.so \
     $B/src/mapi/es2api/libGLESv2_mesa.so $D/prebuilt/mesa/$L/egl/
  cp $B/src/mapi/shared-glapi/libglapi.so $D/prebuilt/mesa/$L/
  cp $B/src/gallium/targets/dri/kgsl_dri.so $D/prebuilt/mesa/$L/dri/
done
mkdir -p $D/proprietary/firmware
cp ~/cm13-build/vendor-lineage/firmware/{a630_sqe.fw,a640_gmu.bin,a640_zap.b00,a640_zap.b01,a640_zap.b02,a640_zap.elf,a640_zap.mdt} $D/proprietary/firmware/
find $D/prebuilt/mesa $D/proprietary -type f | sort
