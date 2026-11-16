#!/bin/bash
# Enter the Ubuntu 14.04 CM13 build chroot (rootless, via user+mount namespaces).
B=$HOME/cm13-build
# The device tree is bind-mounted from this repository.
DEV="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)/device/nubia/TP1803"
exec unshare --map-auto --map-root-user --mount --pid --fork bash -c '
set -e
R='"$B"'/rootfs
mount --rbind /dev $R/dev
mount -t proc proc $R/proc
mount --rbind /sys $R/sys
mkdir -p $R/src $R/ccache
mount --bind '"$B"'/src $R/src
mount --bind '"$B"'/ccache $R/ccache
mkdir -p '"$B"'/src/device/nubia/TP1803
mount --bind '"$DEV"' $R/src/device/nubia/TP1803
cp /etc/resolv.conf $R/etc/resolv.conf
exec chroot $R /usr/bin/env -i HOME=/root TERM=xterm PATH=/root/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C USE_CCACHE=1 CCACHE_DIR=/ccache USER=root "$@"
' bash "$@"
