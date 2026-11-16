# Paranoid Android 6 (AOSPA, Android 6.0.1) for the Nubia Mini 5G (TP1803)

> **Branch `aospa-6`: work in progress.** This branch ports the device to
> AOSPA's `marshmallow-caf-release` (PA 6.0.3, CAF base). It builds a
> complete system image and flashable zip that **boots** (launcher, GPU,
> LTE data, Wi-Fi, Bluetooth, sensors, cameras detected); calls, SMS,
> audio, camera use and fingerprint are not tested on AOSPA yet.
> The finished CyanogenMod 13 port is on the `cm-13` branch.

An unofficial port of Paranoid Android 6 to the Nubia Mini 5G (TP1803,
Snapdragon 855 / msmnile), a phone that launched with Android 9. It runs on
the LineageOS 4.14 kernel. Missing vendor pieces are either replaced with
open source code (Mesa freedreno for the GPU, our own QMI RIL, the
linux-msm daemons) or bridged so the stock blobs run on Android 6.

Two flavours are built from the same tree:

| Variant | Zip | Contents |
|---|---|---|
| vanilla | `pa-6.0.3-YYYYMMDD-TP1803.zip` | AOSPA only |
| gapps | `pa-6.0.3-YYYYMMDD-TP1803-gapps.zip` | AOSPA + OpenGApps 6.0 arm64 **nano** built into `/system` |

## Status

The table below is the state of the CM 13 port (`cm-13` branch). The
device code is the same on this branch, but none of it has been verified on
AOSPA yet.

| Works on CM 13 | Notes |
|---|---|
| Display, GPU | Mesa 24.1 freedreno (Gallium, KGSL backend), OpenGL ES 3.2; glmark2 score 892, stable under 15 min of sustained load |
| Wi-Fi, Bluetooth | Bluetooth through our own `libbt-vendor` for the WCN399x (A2DP tested) |
| Calls, SMS, mobile data | Own QMI-over-QRTR RIL (`device/nubia/TP1803/ril`); LTE, IPv4 data |
| Audio | Speaker, earpiece, mic, in-call audio (stock ACDB calibration + ADSP audio PD) |
| Camera | Stock CamX HAL; both cameras, photo and preview |
| Fingerprint | Stock Sunwave HAL behind a HAL 2.0 shim, QSEE trustlet |
| Sensors, deep sleep, charging | |

Known gaps: no IPv6 on mobile data, no VoLTE/IMS, video decoding falls back
to software in byte-buffer mode, SELinux is disabled (the CM 13 policy can't
load on 4.14), and builds are signed with test keys.

## Repository layout

```
device/nubia/TP1803/   device tree (bind-mounted into the source tree)
  pa_TP1803.mk         AOSPA product (lunch pa_TP1803-userdebug)
  ril/                 QMI RIL (SIM, network, voice, SMS, data)
  fingerprint/         HAL 2.0 shim around the stock Sunwave HAL
  audio/ camera/ bluetooth/ gralloc/ sensors/ ...
  proprietary/         stock blobs (not in git, see "Blobs and prebuilts")
  prebuilt/            kernel, dtb, Mesa and daemon builds (partly not in git)
patches/<project>/     changes to AOSPA projects and external sources
patches/aospa-local-manifest.xml   fixes for AOSPA's dead manifest remotes
scripts/               build, boot image and flashable zip scripts
tools/                 helper builds, blob extraction and test programs
```

## Build environment

Everything below assumes this directory layout on a Linux host:

```
~/cm13-build/
  rootfs/        Ubuntu 14.04 chroot (OpenJDK 7, Android 6 build dependencies)
  src/           CM 13 source tree (cm-13 branch)
  ccache/        shared by both trees
  enter.sh  ->   <this repo>/scripts/enter-chroot.sh
  gapps/         OpenGApps zip (gapps variant only)
~/aospa-build/
  src/           AOSPA source tree (this branch)
  build.log
```

`enter-chroot.sh` mounts `$TP_SRC` as `/src` (default
`~/cm13-build/src`); the scripts on this branch set
`TP_SRC=~/aospa-build/src`.

Host requirements: an unprivileged user-namespace capable kernel
(`unshare --map-root-user`), `python3`, `curl`, `unzip`, `mkbootimg`
(header v2 capable, e.g. from `android-tools`), and `adb`. Budget roughly
150 GB of disk for the source, `out/` and ccache.

### 1. Chroot

The chroot is entered rootless through user and mount namespaces, so no
root is needed on the host.

```sh
mkdir -p ~/cm13-build/rootfs ~/cm13-build/ccache
curl -LO https://cdimage.ubuntu.com/ubuntu-base/releases/14.04/release/ubuntu-base-14.04.6-base-amd64.tar.gz
tar -xzf ubuntu-base-14.04.6-base-amd64.tar.gz -C ~/cm13-build/rootfs
ln -s "$PWD/scripts/enter-chroot.sh" ~/cm13-build/enter.sh

~/cm13-build/enter.sh bash -c 'apt-get update && apt-get -y upgrade && \
  apt-get install -y --no-install-recommends openjdk-7-jdk git-core gnupg flex bison \
  gperf build-essential zip curl zlib1g-dev gcc-multilib g++-multilib libc6-dev-i386 \
  lib32ncurses5-dev x11proto-core-dev libx11-dev lib32z-dev ccache libgl1-mesa-dev \
  libxml2-utils xsltproc unzip python python-dev bc schedtool lzop imagemagick \
  libncurses5-dev libssl-dev rsync ca-certificates pngcrush maven wget xz-utils cpio \
  kmod device-tree-compiler lzip'
```

`enter-chroot.sh` bind-mounts the source tree as `/src` and this
repository's `device/nubia/TP1803` as `/src/device/nubia/TP1803`, so edits
here are built directly.

### 2. Source

Sync AOSPA on the host (the chroot's git and Python are too old for
current `repo`). The stock manifest no longer syncs, so first install
`patches/aospa-local-manifest.xml`. It does three things:

- drops `vendor/head`, which was on AOSPA's private server;
- fetches the 25 CAF projects from CodeLinaro, since codeaurora.org shut
  down in 2022. Each uses the manifest's tag where CodeLinaro mirrors it,
  otherwise the newest Marshmallow 8x96 (`LA.HB.1.3.x`) ref.
- `frameworks/av` comes from `vendor/pa/products/TP1803/pa.dependencies`
  (`patches/vendor_pa`), which `lunch` reads through roomservice.

```sh
mkdir -p ~/aospa-build/src && cd ~/aospa-build/src
repo init -u https://github.com/AOSPA/manifest.git -b marshmallow-caf-release --depth=1
mkdir -p .repo/local_manifests
cp <this repo>/patches/aospa-local-manifest.xml .repo/local_manifests/tp1803.xml
repo sync -c -j8 --no-tags --no-clone-bundle
```

After applying the patches below, run `lunch` once inside the chroot. It
writes `.repo/local_manifests/roomservice.xml` and then fails to sync,
because `repo` doesn't run in the chroot. Sync once more on the host:
`repo sync -c --no-tags frameworks/av`.

### 3. Patches

Apply each patch set to its project. The patch files are plain diffs, so
use `git apply`:

| Patch directory | Apply in |
|---|---|
| `patches/bionic` | `src/bionic` |
| `patches/frameworks_av` | `src/frameworks/av` |
| `patches/frameworks_base` | `src/frameworks/base` |
| `patches/frameworks_opt_net_wifi` | `src/frameworks/opt/net/wifi` |
| `patches/hardware_libhardware_legacy` | `src/hardware/libhardware_legacy` |
| `patches/libcore` | `src/libcore` |
| `patches/build` | `src/build` |
| `patches/frameworks_native` | `src/frameworks/native` |
| `patches/vendor_pa` | `src/vendor/pa` |
| `patches/system_core` | `src/system/core` |
| `patches/system_media` | `src/system/media` |
| `patches/system_netd` | `src/system/netd` |
| `patches/mesa` | Mesa source (`tools/mesa`) |
| `patches/hexagonrpc` | hexagonrpc source (`tools/hexagonrpc`) |
| `patches/tqftpserv` | tqftpserv source (`tools/qcom-daemons`) |

```sh
S=~/aospa-build/src; P="$PWD/patches"
for pair in bionic:bionic build:build frameworks_base:frameworks/base \
    frameworks_native:frameworks/native frameworks_opt_net_wifi:frameworks/opt/net/wifi \
    hardware_libhardware_legacy:hardware/libhardware_legacy libcore:libcore \
    system_core:system/core system_media:system/media system_netd:system/netd \
    vendor_pa:vendor/pa; do
  git -C "$S/${pair#*:}" apply "$P/${pair%%:*}"/*.patch
done
# after frameworks/av is synced (see "Source"):
git -C "$S/frameworks/av" apply "$P"/frameworks_av/*.patch
```

### 4. Blobs and prebuilts

These are not tracked in git (`device/nubia/TP1803/proprietary/`,
`device/nubia/TP1803/prebuilt/mesa/`):

- **Kernel and dtb:** `prebuilt/Image.gz` and `prebuilt/dtb` are taken from
  a LineageOS 4.14 TP1803 `boot.img`.
- **Stock blobs:** extract the LineageOS TP1803 vendor partition to
  `~/cm13-build/vendor-lineage`. `tools/camera/extract-blobs.sh` copies the
  camera closure. The audio (ACDB loader and data), fingerprint (Sunwave HAL
  with its SONAME patched to `libsunwave_fp.so`, `libQSEEComAPI`,
  `qseecomd` and listener libraries), Wi-Fi, sensor and firmware blobs go
  under `proprietary/`, as listed in `device.mk`.
- **Mesa:** `tools/mesa/build-mesa.sh aarch64`, `build-mesa.sh arm`, then
  `tools/mesa/install-prebuilts.sh`.
- **Daemons:** `tools/qcom-daemons/build.sh` (qrtr-ns, pd-mapper, rmtfs,
  tqftpserv, modem helpers) and `tools/hexagonrpc/build.sh` (FastRPC for
  the sensor and audio DSPs).
- **WebView:** `tools/webview/fetch-webview-m106.sh` replaces the tree's prebuilt
  WebView (AOSPA ships 44) with 106, the last version that supports Android 6.
  The root CA additions are in `patches/libcore`.

Several helpers use NDK r27d at
`~/cm13-build/gpu/android-ndk-r27d`.

## Building

All commands run on the host from this repository. `scripts/build.sh`
enters the chroot itself and writes the log to `~/aospa-build/build.log`.

### Vanilla

```sh
scripts/build.sh systemimage target-files-package   # ~7 min incremental
scripts/mkboot.sh                                    # boot-v2.img
scripts/mkota.sh                                     # flashable zip
```

Output: `~/aospa-build/src/out/dist-tp1803/pa-6.0.3-YYYYMMDD-TP1803.zip`.

Notes:

- `mkboot.sh` repacks the ROM's ramdisk with the prebuilt kernel and dtb
  into a header v2 boot image, which the TP1803 bootloader requires. The
  build's own `boot.img` does not boot.
- `make ramdisk` doesn't notice changed `rootdir/*.rc` files. After editing
  them, copy them into `out/target/product/TP1803/root/` and repack inside
  the chroot:

  ```sh
  TP_SRC=~/aospa-build/src ~/cm13-build/enter.sh bash -c 'cd /src/out/target/product/TP1803 && \
    /src/out/host/linux-x86/bin/mkbootfs -d system root | \
    /src/out/host/linux-x86/bin/minigzip > ramdisk.img'
  ```

  Then run `scripts/mkboot.sh` again.
- `mkota.sh` takes the newest target-files package, puts `boot-v2.img` in
  place of the build's `boot.img`, and runs `ota_from_target_files --block`.
  The Android 6 static updater can't read properties under the Android 10
  based recovery, so `mkota.sh` then fixes the generated script:
  - it swaps AOSPA's `get_device_compatible()` device check for one the
    recovery can evaluate;
  - it drops the downgrade check (`ro.build.date.utc`).

  Finally it re-signs the zip with the test key.
- The staging directory `out/target/product/TP1803/system` is packed as-is,
  so remove stray files from it before building an image.

### GApps

Download OpenGApps arm64 / 6.0 / nano. 20220503 is the last build:

```sh
mkdir -p ~/cm13-build/gapps && cd ~/cm13-build/gapps
curl -LO https://downloads.sourceforge.net/project/opengapps/arm64/20220503/open_gapps-arm64-6.0-nano-20220503.zip
curl -LO https://downloads.sourceforge.net/project/opengapps/arm64/20220503/open_gapps-arm64-6.0-nano-20220503.zip.md5
md5sum -c open_gapps-arm64-6.0-nano-20220503.zip.md5
```

Then, after the vanilla build steps above (`build.sh` and `mkboot.sh`):

```sh
scripts/mkgapps.sh
```

Output: `~/aospa-build/src/out/dist-tp1803/pa-6.0.3-YYYYMMDD-TP1803-gapps.zip`.

`mkgapps.sh` unpacks the OpenGApps tarballs in the chroot (with `lzip`).
`scripts/add-gapps.py` then adds them to a copy of the vanilla target-files
package and records their permissions. `mkota.sh` rebuilds the system image
from it, so the vanilla zip and build output are untouched. The package
selection mirrors the OpenGApps installer on this phone:

- all nano packages except the tablet setup wizard and Google's
  PackageInstaller (the OpenGApps installer skips that on CM 13; the same
  selection is used on AOSPA for now);
- the `nodpi` GmsCore (no 420 dpi build);
- `Provision` removed;
- Swype libraries linked into LatinIME.

There is no `addon.d` script, because the GApps are part of the image.

## Installing

Use the LineageOS recovery for TP1803 (flashed to the `recovery`
partition).

1. Reboot to recovery: `adb reboot recovery`.
2. **First install, or switching between vanilla and gapps:** wipe data.
   This ROM's `/data` is ext4, while the recovery's fstab expects f2fs, so
   either use "Factory reset" in recovery or wipe from `adb shell`:

   ```sh
   mount -t ext4 /dev/block/bootdevice/by-name/userdata /data
   cd /data && for d in * .layout_version; do
     case $d in media|lost+found) ;; *) rm -rf "/data/$d";; esac; done
   ```

   This keeps internal storage (`/data/media`).
3. Choose **Apply update → Apply from ADB**, then run
   `adb sideload pa-6.0.3-YYYYMMDD-TP1803[-gapps].zip`. Recovery warns that
   the signature can't be verified (test keys); choose **Yes** to continue.
   The "liblp ... invalid geometry" errors it prints are harmless.

The zip writes `system` and `boot` only. It never touches the recovery
partition, and it does not wipe data by itself.

## Testing helpers

- `tools/ril/`: data bring-up (`data-test`) and SMS/QMI probes.
- `tools/fingerprint/fp-test`: drive the fingerprint HAL directly
  (`enroll`, `auth`, `enum`, `remove`).
- `tools/camera/camx-probe`: open the CamX HAL without the framework.
- `tools/audio/`: voice-path and mixer test programs.
- glmark2 for Android 6: native library built with NDK r27d (C++17),
  packaged in the CM 13 tree (`cm-13` branch). Run it with
  `am start -n org.linaro.glmark2/.Glmark2Activity -e args "..."`. Results
  go to `/data/data/org.linaro.glmark2/files/last_run.log`.

Kernel GPU and thermal state for stress tests is in
`/sys/class/kgsl/kgsl-3d0/` (`gpuclk`, `gpu_busy_percentage`, `temp`,
`thermal_pwrlevel`).
