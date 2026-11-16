#!/bin/bash
# usage: build.sh [make targets...]   (run on host; builds inside the chroot)
# AOSPA (Paranoid Android 6) tree: ~/aospa-build/src, product pa_TP1803.
# Log: ~/aospa-build/build.log
T="${*:-bootimage systemimage userdataimage cacheimage}"
SRC=${TP_SRC:-$HOME/aospa-build/src}
LOG=$(dirname "$SRC")/build.log
cd ~/cm13-build && TP_SRC="$SRC" ./enter.sh bash -c "cd /src && export USE_CCACHE=1 ANDROID_JACK_VM_ARGS='-Xmx4g -Dfile.encoding=UTF-8 -XX:+TieredCompilation'; source build/envsetup.sh >/dev/null && lunch pa_TP1803-userdebug >/dev/null && make -j10 $T" > "$LOG" 2>&1
echo exit=$? >> "$LOG"
