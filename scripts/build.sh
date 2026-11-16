#!/bin/bash
# usage: build.sh [make targets...]   (run on host; builds inside the chroot)
T="${*:-bootimage systemimage userdataimage cacheimage}"
SRC=~/cm13-build/src
# Pre-fetch Maven artifacts on the host: the chroot's Java 7 maven can't do modern TLS.
AAR=$SRC/out/target/common/obj/JAVA_LIBRARIES/ambientsdk_intermediates/com.cyngn.ambient.ambientsdk-1.6.0-147069776-39fe2d07.aar
if [ ! -s "$AAR" ]; then
  mkdir -p "$(dirname "$AAR")"
  curl -sfL -o "$AAR" https://repo1.maven.org/maven2/com/cyngn/ambient/ambientsdk/1.6.0-147069776-39fe2d07/ambientsdk-1.6.0-147069776-39fe2d07.aar || { echo "aar fetch failed"; exit 1; }
fi
cd ~/cm13-build && ./enter.sh bash -c "cd /src && export USE_CCACHE=1 ANDROID_JACK_VM_ARGS='-Xmx4g -Dfile.encoding=UTF-8 -XX:+TieredCompilation'; source build/envsetup.sh >/dev/null && lunch cm_TP1803-userdebug >/dev/null && make -j10 $T" > build.log 2>&1
echo exit=$? >> build.log
