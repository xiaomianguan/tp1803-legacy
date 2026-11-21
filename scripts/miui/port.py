#!/usr/bin/env python3
"""Port MIUI 7 (Xiaomi Mi 5 "gemini", Android 6.0) to the Nubia TP1803.

usage: port.py <miui zip> <aospa src> <work dir> <copyfiles.txt> <props.txt>

Takes MIUI's system image as is (it is odexed against its own boot
classpath, so framework jars must not change) and patches it in place with
debugfs, keeping Xiaomi's ownership/permissions:

  - removes the Mi 5 (msm8996) HAL modules and Adreno 530 EGL drivers
  - adds our device payload from the AOSPA build (out/target/product/TP1803):
    /system/vendor, our HALs (*.msmnile, camera.tp1803, fingerprint.qcom),
    Mesa, daemons, configs, firmware (PRODUCT_COPY_FILES from our device tree)
  - replaces bionic, libutils, libcutils, libsync with ours (supersets of
    MIUI's exports; needed by the CamX/Sunwave blobs and the sync_file fix)
    and wpa_supplicant (MIUI's needs Mi 5 vendor libs)
  - binary-patches MIUI's libsurfaceflinger for GLES-only composition
    (real blur without an HWC; no QCOM gralloc handle flag checks)
  - rewrites build.prop: msmnile board/platform and our device properties

The ramdisk is our AOSPA userdebug ramdisk (MIUI's user-build init forces
SELinux, which can't load on 4.14) with MIUI's init.rc/init.environ.rc/
init.miui.rc/zygote scripts, and the boot image uses our 4.14 kernel.
"""
import gzip, os, re, shutil, struct, subprocess, sys, zipfile

zip_path, aospa, work, copyfiles_txt, props_txt = sys.argv[1:]
OUT = os.path.join(aospa, "out/target/product/TP1803")
SYS = os.path.join(OUT, "system")
ROOT = os.path.join(OUT, "root")
DEV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../device/nubia/TP1803")
BS = 4096
os.makedirs(work, exist_ok=True)

def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)

# ---------------------------------------------------------------- system image
img = os.path.join(work, "system.img")
mi = os.path.join(work, "miui")
os.makedirs(mi, exist_ok=True)
with zipfile.ZipFile(zip_path) as z:
    for n in ("system.new.dat", "system.transfer.list", "boot.img"):
        z.extract(n, mi)

def sdat2img(tl, dat, out):
    lines = open(tl).read().split("\n")
    ver = int(lines[0])
    rng = lambda s: (lambda a: [(a[i], a[i + 1]) for i in range(1, len(a), 2)])([int(x) for x in s.split(",")])
    cmds = [l.split(" ", 1) for l in lines[(4 if ver >= 2 else 2):] if l.strip()]
    maxb = max(e for c, a in cmds if c in ("new", "zero", "erase") for s, e in rng(a))
    with open(dat, "rb") as d, open(out, "wb") as o:
        o.truncate(maxb * BS)
        for c, a in cmds:
            if c == "new":
                for s, e in rng(a):
                    o.seek(s * BS)
                    n = (e - s) * BS
                    while n:
                        b = d.read(min(n, 1 << 24))
                        o.write(b)
                        n -= len(b)
    return maxb

blocks = sdat2img(os.path.join(mi, "system.transfer.list"), os.path.join(mi, "system.new.dat"), img)
print("MIUI system image: %d blocks" % blocks)

def miui_ls(path):
    r = subprocess.run(["debugfs", "-R", "ls -p %s" % path, img], capture_output=True, text=True).stdout
    names = []
    for l in r.splitlines():
        f = l.strip("/").split("/")
        # /inode/mode/uid/gid/name/size/
        if len(f) >= 5 and f[4] not in (".", ".."):
            names.append(f[4])
    return names

def miui_cat(path):
    return subprocess.run(["debugfs", "-R", "cat %s" % path, img], capture_output=True).stdout

# ---------------------------------------------------------------- payload
payload = {}       # dest under /system -> host source
def add(rel):
    src = os.path.join(SYS, rel)
    if os.path.lexists(src):
        payload[rel] = src

# 1. everything our device tree copies into /system (PRODUCT_COPY_FILES)
for tok in open(copyfiles_txt).read().split():
    if ":" not in tok:
        continue
    src, dst = tok.split(":")[:2]
    if src.startswith(("device/nubia/TP1803/", "frameworks/native/data/etc/")) and dst.startswith("system/"):
        add(dst[len("system/"):])
# 2. all of our /system/vendor (blobs, stubs, RIL, OMX, qseecomd, ...)
for root, dirs, files in os.walk(os.path.join(SYS, "vendor")):
    for f in files + [d for d in dirs if os.path.islink(os.path.join(root, d))]:
        add(os.path.relpath(os.path.join(root, f), SYS))
# 3. our HAL modules, Mesa, and replaced core libraries
for arch in ("lib", "lib64"):
    for f in os.listdir(os.path.join(SYS, arch, "hw")):
        if f.endswith((".msmnile.so", ".tp1803.so")) or f == "fingerprint.qcom.so":
            add("%s/hw/%s" % (arch, f))
    for sub in ("egl", "dri"):
        d = os.path.join(SYS, arch, sub)
        for f in os.listdir(d) if os.path.isdir(d) else []:
            if "mesa" in f or sub == "dri":
                add("%s/%s/%s" % (arch, sub, f))
    # libaudioroute: ours skips mixer paths with controls the codec lacks
    # (patches/system_media); MIUI's crashes mediaserver on them.
    # libhardware_legacy: ours loads the built-in WLAN driver the 4.14 way and
    # falls back to the vibrator HAL (patches/hardware_libhardware_legacy);
    # MIUI's only adds read_wlan_mac_addr, used by nothing outside it.
    for f in ("libc.so", "libm.so", "libdl.so", "libutils.so", "libcutils.so", "libsync.so",
              "libaudioroute.so", "libhardware_legacy.so"):
        add("%s/%s" % (arch, f))
# netd: ours uses the upstream FRA_UID_RANGE rule attribute (patches/system_netd);
# MIUI's sends FRA_UID_START/END, which 4.14 rejects -> no per-network routing
# rules, "Network is unreachable" and "No Internet Access" on Wi-Fi.
for f in ("bin/linker", "bin/linker64", "bin/wpa_supplicant", "bin/netd"):
    add(f)
# The WLAN driver requests wlan/qca_cld/WCNSS_qcom_cfg.ini and the firmware
# loader finds MIUI's /system/etc/firmware/wlan/qca_cld/ symlink (to
# /system/etc/wifi/WCNSS_qcom_cfg.ini) before our /vendor copy. The Mi 5 ini
# has gScanAgingTime=0, so every scan result was aged out before a connect
# could use it (ASSOC-REJECT on every AP). Serve our ini at that path too.
payload["etc/wifi/WCNSS_qcom_cfg.ini"] = os.path.join(SYS, "vendor/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini")

# 4. MIUI's own SurfaceFlinger, binary-patched for a phone without a
#    Qualcomm hwcomposer/gralloc (our display is GLES composition over KMS).
#    The ELF maps file offset 0 at vaddr 0x13000; each word is checked first.
SF_BASE = 0x13000
SF_PATCHES = [
    # setUpHWComposer returns right after hwc.initCheck() fails, before it
    # computes whether a layer wants MIUI's real blur, so the shade and recents
    # never blurred. Fall through to that computation (the work-list helpers
    # are no-ops without an HWC)...
    (0x33470, 0x35002da0, 0xd503201f),   # cbnz w0, <return>  -> nop
    # ...and return before hwc.prepare(), which would dereference the missing HAL.
    (0x33994, 0xaa1403e0, 0x14000024),   # mov x0, x20        -> b <return>
]
# CAF's ExLayer::isExtOnly/isIntOnly/isSecureDisplay/isYuvLayer read flag bits
# at fixed offsets of a QCOM private_handle_t. Our gralloc's handle has another
# layout, so random buffers (e.g. Settings' Wi-Fi password dialog) looked
# "external only" and computeVisibleRegions dropped them. No external display
# here: make all four return false.
for va in (0x4e458, 0x4e4ac, 0x4e500, 0x4e554):
    SF_PATCHES += [(va, 0xa9bf7bfd, 0x2a1f03e0),        # stp x29, x30 -> mov w0, wzr
                   (va + 4, 0x910003fd, 0xd65f03c0)]    # mov x29, sp  -> ret
sf = bytearray(miui_cat("/lib64/libsurfaceflinger.so"))
for va, old, new in SF_PATCHES:
    cur = struct.unpack_from("<I", sf, va - SF_BASE)[0]
    if cur != old:
        sys.exit("libsurfaceflinger: unexpected word %#x at %#x" % (cur, va))
    struct.pack_into("<I", sf, va - SF_BASE, new)
open(os.path.join(work, "libsurfaceflinger.so"), "wb").write(sf)
payload["lib64/libsurfaceflinger.so"] = os.path.join(work, "libsurfaceflinger.so")

def put_bytes(rel, data):
    """Payload entry generated here (written to the work dir)."""
    p = os.path.join(work, "gen", rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)
    payload[rel] = p

# 5. WebView 106 (our com.android.webview) instead of MIUI's Google WebView
#    46. MIUI's framework-res names com.google.android.webview as the provider
#    and its resource loader can't take a runtime overlay, so the string is
#    patched in resources.arsc (M verifies only the manifest of system APKs,
#    so Xiaomi's platform signature stays valid).
fr_in = os.path.join(work, "framework-res.miui.apk")
fr_out = os.path.join(work, "framework-res.unaligned.apk")
open(fr_in, "wb").write(miui_cat("/framework/framework-res.apk"))
run(sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "arsc_patch.py"),
    fr_in, fr_out, "com.google.android.webview=com.android.webview")
run(os.path.join(aospa, "out/host/linux-x86/bin/zipalign"), "-f", "4", fr_out,
    os.path.join(work, "framework-res.apk"))
payload["framework/framework-res.apk"] = os.path.join(work, "framework-res.apk")
add("app/webview/webview.apk")

# 6. Root CAs: ours (Mozilla 2022, 243) replace MIUI's 2016 store (158)
for f in os.listdir(os.path.join(SYS, "etc/security/cacerts")):
    add("etc/security/cacerts/" + f)

# 7. Camera. CamX is 64-bit only, so CameraService runs in our cameraserver
#    (init.qcom.rc) as on CM/AOSPA. libcameraservice_miui is our service built
#    for MIUI 7's ICamera layout (patches/frameworks_av/miui-icamera-abi.patch:
#    two extra calls, transaction codes 12/13) against MIUI's libcamera_client;
#    build it with `scripts/build.sh libcameraservice_miui`. MIUI's 32-bit
#    mediaserver still constructs its own CameraService: its registration
#    name is renamed so ours owns media.camera.
cs_miui = os.path.join(SYS, "lib64/libcameraservice_miui.so")
if not os.path.exists(cs_miui):
    sys.exit("missing %s: run scripts/build.sh libcameraservice_miui" % cs_miui)
payload["lib64/libcameraservice.so"] = cs_miui
for f in ("bin/cameraserver", "lib64/libmediautils.so",
          # Q camera metadata (CamX tags), both ABIs as on CM
          "lib64/libcamera_metadata.so", "lib/libcamera_metadata.so",
          # recording: our CameraSource/StagefrightRecorder take the frames
          # cameraserver sends over a dma-buf fd (patches/frameworks_av);
          # MIUI's pair assumes an in-process camera and crashes mediaserver
          "lib/libstagefright.so", "lib/libmediaplayerservice.so"):
    add(f)
ms = bytearray(miui_cat("/bin/mediaserver"))
if ms.count(b"media.camera\0") != 1:
    sys.exit("mediaserver: media.camera string not found exactly once")
i = ms.index(b"media.camera\0")
ms[i:i + 13] = b"media.cam_mi\0"
put_bytes("bin/mediaserver", bytes(ms))

# 8. Codecs: our media_codecs.xml includes media_codecs_ffmpeg.xml, and a
#    missing include makes MediaCodecList drop the whole list (no codecs at
#    all: no UI sounds, no recording). List MIUI's own FFmpeg decoders there.
put_bytes("etc/media_codecs_ffmpeg.xml", b"""<?xml version="1.0" encoding="utf-8" ?>
<!-- MIUI's FFmpeg OMX decoders (libstagefright_soft_ffmpegadec), included by
     our media_codecs.xml (a missing include empties the whole codec list) -->
<Included>
    <Decoders>
        <MediaCodec name="OMX.ffmpeg.dsd.decoder" type="audio/dsd" />
        <MediaCodec name="OMX.ffmpeg.adpcm.decoder" type="audio/32KADPCM" />
    </Decoders>
</Included>
""")

# 9. MIUI device features (gemini.xml, kept as ro.product.device=gemini):
#    describe the TP1803 instead of the Mi 5. vendor!=qcom makes MIUI Camera
#    use its generic API1 path: its Qualcomm path expects the Mi 5's HAL1
#    vendor parameters (max-saturation...) that a HAL3 camera doesn't have.
feat = miui_cat("/etc/device_features/gemini.xml").decode()
FEATURES = {
    "vendor": "nubia",                       # string
    "support_ir": "false", "support_hall_sensor": "false",
    "support_led_light": "false", "support_button_light": "false",
    "support_fm": "false", "support_small_win_cover": "false",
    "front_fingerprint_sensor": "false",
    "support_tap_fingerprint_sensor_to_home": "false",
}
for k, v in FEATURES.items():
    feat, n = re.subn(r'(<(bool|string) name="%s">)[^<]*(</\2>)' % k, r"\g<1>%s\3" % v, feat)
    if n != 1:
        sys.exit("gemini.xml: feature %s found %d times" % (k, n))
put_bytes("etc/device_features/gemini.xml", feat.encode())

# 10. GApps variant (port.sh GAPPS=...): OpenGApps 6.0 arm64 nano, the same
#     package and selection as the CM/AOSPA images (scripts/add-gapps.py),
#     except: no Google SetupWizard (MIUI's Provision runs first boot; add
#     the account in Settings), no Google PackageInstaller (MIUI's installer
#     is tied into its security scan) and no swypelibs (MIUI has no LatinIME).
GAPPS_PACKAGES = [
    "configupdater-all/nodpi", "defaultetc-common/common", "defaultframework-common/common",
    "gmscore-arm64/nodpi", "googlebackuptransport-all/nodpi", "googlecontactssync-all/nodpi",
    "googlefeedback-all/nodpi", "googleonetimeinitializer-all/nodpi",
    "googlepartnersetup-all/nodpi", "gsfcore-all/nodpi", "gsflogin-all/nodpi",
    "vending-arm64/nodpi", "calsync-all/nodpi", "dialerframework-common/common",
    "googletts-arm64/nodpi", "search-arm64/nodpi", "speech-common/common",
]
gapps_dir = os.environ.get("MIUI_GAPPS")
if gapps_dir:
    _ls = {}
    def miui_exists(rel):
        parent, name = os.path.split(rel)
        if parent not in _ls:
            _ls[parent] = set(miui_ls("/" + parent))
        return name in _ls[parent]
    added = 0
    for pkg in GAPPS_PACKAGES:
        root = os.path.join(gapps_dir, "tars", pkg)
        if not os.path.isdir(root):
            sys.exit("gapps: missing package %s" % pkg)
        for d, _, names in os.walk(root):
            for n in names:
                rel = os.path.relpath(os.path.join(d, n), root)
                if miui_exists(rel):
                    # MIUI already ships it (com.google.android.maps): keep
                    # MIUI's copy, which the rest of the ROM was built with
                    print("gapps: keeping MIUI's %s" % rel)
                    continue
                payload[rel] = os.path.join(d, n)
                added += 1
    payload["etc/g.prop"] = os.path.join(gapps_dir, "pkg", "g.prop")
    print("gapps: %d files from %d packages" % (added + 1, len(GAPPS_PACKAGES)))
print("payload: %d files" % len(payload))

# ---------------------------------------------------------------- removals
removals = []
for arch in ("lib", "lib64"):
    for d in ("%s/hw" % arch, "vendor/%s/hw" % arch):
        for f in miui_ls("/" + d):
            if "msm8996" in f or f in ("fingerprint.fpc.so", "fingerprint.qcom.so"):
                removals.append("%s/%s" % (d, f))
    for f in miui_ls("/vendor/%s/egl" % arch):
        removals.append("vendor/%s/egl/%s" % (arch, f))
# Mi 5 hardware this phone lacks. consumerir is fatal: ConsumerIrService
# throws without an IR HAL and system_server never creates WindowManager.
for f in ("android.hardware.consumerir.xml", "android.hardware.sensor.barometer.xml",
          "android.hardware.sensor.stepcounter.xml", "android.hardware.sensor.stepdetector.xml",
          "android.hardware.sensor.hifi_sensors.xml",
          # Wi-Fi Direct makes the framework start "p2p_supplicant"; our init
          # (like CM/AOSPA) runs a plain wpa_supplicant service
          "android.hardware.wifi.direct.xml"):
    removals.append("etc/permissions/" + f)
# MIUI's flash_recovery would rewrite our recovery partition from boot
removals += ["recovery-from-boot.p", "bin/install-recovery.sh"]
# NFC: the TP1803's NFC controller doesn't answer ("NFCC HW not available")
for f in ("android.hardware.nfc.xml", "android.hardware.nfc.hce.xml", "com.android.nfc_extras.xml"):
    removals.append("etc/permissions/" + f)
# Mi 5 Qualcomm clients that misbehave here:
#  - power.qcom (ro.hardware=qcom picks it first) drives the Mi 5's perfd;
#    power.default takes over
#  - libqti-perfd-client: CamX's CHI dlopens it and calls a NULL entry
#    (cameraserver SIGSEGV); without it CHI just skips perf locks
#  - libavenhancements: MIUI's AV extensions, built against MIUI's
#    libstagefright (which ours replaces)
removals += ["lib/hw/power.qcom.so", "lib64/hw/power.qcom.so",
             "vendor/lib/libqti-perfd-client.so", "vendor/lib64/libqti-perfd-client.so",
             "vendor/lib/libavenhancements.so"]
# CA store: drop MIUI certificates that aren't in ours
ours_ca = set(os.listdir(os.path.join(SYS, "etc/security/cacerts")))
removals += ["etc/security/cacerts/" + f for f in miui_ls("/etc/security/cacerts") if f not in ours_ca]
# Mi 5 apps that hang without their Qualcomm daemons. com.qualcomm.location
# (IZat, persistent, system uid) blocks on its location service -> ANR in
# the system process. WebViewGoogle: replaced by our WebView 106. NFC apps:
# no NFC hardware.
REMOVE_DIRS = ["priv-app/com.qualcomm.location", "app/WebViewGoogle", "app/NfcNci", "app/NFCtestSvc"]
print("removals: %d" % len(removals))

# ---------------------------------------------------------------- build.prop
props = {}
for tok in open(props_txt).read().split():
    if "=" in tok:
        k, v = tok.split("=", 1)
        props[k] = v
for line in open(os.path.join(DEV, "system.prop")):
    line = line.strip()
    if line and not line.startswith("#") and "=" in line:
        k, v = line.split("=", 1)
        props[k] = v
# The Mi 5 has hardware navigation keys, the TP1803 doesn't: force the
# on-screen navigation bar (overrides framework config_showNavigationBar)
props["qemu.hw.mainkeys"] = "0"
props["ro.product.board"] = "msmnile"
props["ro.board.platform"] = "msmnile"
bp = miui_cat("/build.prop").decode("utf-8", "replace").split("\n")
bp = [l for l in bp if not (("=" in l) and l.split("=", 1)[0].strip() in props)]
bp += ["", "# TP1803 port (Nubia Mini 5G, SM8150)"] + ["%s=%s" % kv for kv in sorted(props.items())]
open(os.path.join(work, "build.prop"), "w").write("\n".join(bp) + "\n")

# ---------------------------------------------------------------- debugfs script
cmds = []
made = set()
_listing = {}
def miui_has(rel):
    parent, name = os.path.split(rel)
    if parent not in _listing:
        _listing[parent] = set(miui_ls("/" + parent))
    return name in _listing[parent]

def ensure_dir(rel):
    """Create missing directories only; existing ones keep MIUI's metadata."""
    parts = rel.split("/")
    for i in range(1, len(parts) + 1):
        p = "/".join(parts[:i])
        if p in made:
            continue
        made.add(p)
        if miui_has(p):
            continue
        cmds.append("mkdir /%s" % p)
        cmds.append("sif /%s mode 040755" % p)
        cmds.append("sif /%s uid 0" % p)
        cmds.append("sif /%s gid 0" % p)

def is_exec(rel):
    return rel.startswith(("bin/", "xbin/", "vendor/bin/"))

for rel in removals:
    cmds.append("rm /%s" % rel)

def remove_tree(rel):
    for name in miui_ls("/" + rel):
        child = "%s/%s" % (rel, name)
        mode = subprocess.run(["debugfs", "-R", "stat /%s" % child, img], capture_output=True,
                              text=True).stdout
        if "Type: directory" in mode:
            remove_tree(child)
        else:
            cmds.append("rm /%s" % child)
    cmds.append("rmdir /%s" % rel)

for rel in REMOVE_DIRS:
    remove_tree(rel)
for rel, src in sorted(payload.items()):
    d = os.path.dirname(rel)
    if d:
        ensure_dir(d)
    cmds.append("rm /%s" % rel)
    if os.path.islink(src):
        cmds.append("symlink /%s %s" % (rel, os.readlink(src)))
        continue
    cmds.append("write %s /%s" % (src, rel))
    cmds.append("sif /%s mode %s" % (rel, "0100755" if is_exec(rel) else "0100644"))
    cmds.append("sif /%s uid 0" % rel)
    cmds.append("sif /%s gid %d" % (rel, 2000 if is_exec(rel) else 0))
cmds.append("rm /build.prop")
cmds.append("write %s /build.prop" % os.path.join(work, "build.prop"))
cmds.append("sif /build.prop mode 0100644")
cmds.append("sif /build.prop uid 0")
cmds.append("sif /build.prop gid 0")
script = os.path.join(work, "debugfs.cmds")
open(script, "w").write("\n".join(cmds) + "\n")
r = subprocess.run(["debugfs", "-w", "-f", script, img], capture_output=True, text=True)
errs = [l for l in r.stderr.splitlines() if l.strip() and "File not found by ext2_lookup" not in l
        and "already exists" not in l and not l.startswith("debugfs ")]
print("debugfs: %d commands, %d unexpected messages" % (len(cmds), len(errs)))
for l in errs[:20]:
    print("  ", l)
run("e2fsck", "-fn", img, stdout=subprocess.DEVNULL)
print("e2fsck: clean")

# ---------------------------------------------------------------- ramdisk
rd = os.path.join(work, "ramdisk")
shutil.rmtree(rd, ignore_errors=True)
shutil.copytree(ROOT, rd, symlinks=True)
mrd = os.path.join(work, "miui-ramdisk")
shutil.rmtree(mrd, ignore_errors=True)
os.makedirs(mrd)
bootimg = open(os.path.join(mi, "boot.img"), "rb").read()
ks, ka, rs, ra, ss, sa, ta, ps = struct.unpack_from("<8I", bootimg, 8)
koff = ps
roff = koff + (ks + ps - 1) // ps * ps
run("cpio", "-id", "--quiet", cwd=mrd, input=gzip.decompress(bootimg[roff:roff + rs]))
for f in ("init.environ.rc", "init.miui.rc", "init.zygote64_32.rc", "init.zygote32.rc"):
    shutil.copy(os.path.join(mrd, f), os.path.join(rd, f))
shutil.copy(os.path.join(mrd, "sbin/setlockstate"), os.path.join(rd, "sbin/setlockstate"))
# healthd must match MIUI's framework: our 6.0.1 healthd sends maxChargingCurrent
# in BatteryProperties, MIUI's 6.0.0 framework doesn't read it, so every field
# shifts (level 1%, 414 C) and BatteryService shuts down for over-temperature.
shutil.copy(os.path.join(mrd, "sbin/healthd"), os.path.join(rd, "sbin/healthd"))
initrc = open(os.path.join(mrd, "init.rc")).read()
# MIUI's init.rc is Android 6.0.0; our 6.0.1 init split load_all_props
initrc = initrc.replace("    load_all_props\n", "    load_system_props\n    load_persist_props\n")
# USB on the 4.14 kernel is configfs (our init.qcom.usb.rc)
initrc = initrc.replace("import /init.usb.rc\n", "import /init.usb.rc\nimport /init.usb.configfs.rc\n")
assert "load_all_props\n" not in initrc.replace("load_all_props_action", "")
open(os.path.join(rd, "init.rc"), "w").write(initrc)
dp = open(os.path.join(rd, "default.prop")).read().rstrip("\n").split("\n")
for line in open(os.path.join(mrd, "default.prop")):
    if line.startswith("ro.miui."):
        dp.append(line.strip().replace("has_cust_partition=true", "has_cust_partition=false"))
open(os.path.join(rd, "default.prop"), "w").write("\n".join(dp) + "\n")
print("ramdisk composed")
