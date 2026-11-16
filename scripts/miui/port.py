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
    for f in ("libc.so", "libm.so", "libdl.so", "libutils.so", "libcutils.so", "libsync.so"):
        add("%s/%s" % (arch, f))
for f in ("bin/linker", "bin/linker64", "bin/wpa_supplicant"):
    add(f)
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
