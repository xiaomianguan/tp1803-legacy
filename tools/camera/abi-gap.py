#!/usr/bin/env python3
"""Resolve the CamX HAL's dependency closure against CM13's /system and list
libraries and symbols that Android 6 can't provide.
usage: abi-gap.py <vendor root> <cm13 system root> [extra entry libs...]"""
import os, subprocess, sys, glob
RE = os.path.expanduser("~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf")
V, S = sys.argv[1], sys.argv[2]
entries = [os.path.join(V, "lib64/hw/camera.qcom.so"), os.path.join(V, "lib64/hw/com.qti.chi.override.so")]
# Loaded by CamX at runtime (names built at runtime, so not visible as strings)
entries += [os.path.join(V, "lib64", n) for n in (
    "libcamxtintlessalgo.so", "libcamxswprocessalgo.so", "libcamxfdalgov8.so", "libthermalclient.so")] + glob.glob(os.path.join(V, "lib64/camera/components/*.so")) \
    + glob.glob(os.path.join(V, "lib64/camera/*.so")) + sys.argv[3:]
VDIRS = [os.path.join(V, d) for d in ("lib64", "lib64/hw", "lib64/camera", "lib64/camera/components")]
SDIRS = [os.path.join(S, "lib64")]

# Libraries we replace with our own stubs (device/nubia/TP1803/camera/shims)
STUBS = {"libqdMetaData.so": {"setMetaData"},
         "libcdsprpc.so": {"remote_handle_open", "remote_handle_invoke", "remote_handle_close",
                           "remote_handle64_open", "remote_handle64_invoke", "remote_handle64_close",
                           "rpcmem_alloc", "rpcmem_free"}}

# Generated stubs (gen-stubs.py): their exported symbols, from the .S files
import re as _re
_SD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../device/nubia/TP1803/camera/shims/hidl")
for _f in glob.glob(os.path.join(_SD, "*.S")):
    STUBS[os.path.basename(_f)[:-2] + ".so"] = set(_re.findall(r"^\.globl (\S+)", open(_f).read(), _re.M))

def find(name):
    if name in STUBS: return "STUB:" + name, "stub"
    # Libraries CM13's /system already has win: the process has them loaded.
    for dirs, kind in ((SDIRS, "system"), (VDIRS, "vendor")):
        for d in dirs:
            p = os.path.join(d, name)
            if os.path.exists(p): return p, kind
    return None, None

def syms(path):
    if path.startswith("STUB:"): return set(), STUBS[path[5:]]
    out = subprocess.run([RE, "--dyn-syms", "-W", path], capture_output=True, text=True).stdout
    und, dfn = set(), set()
    for l in out.splitlines():
        f = l.split()
        if len(f) < 8 or not f[0].endswith(":"): continue
        name = f[7].split("@")[0]
        if f[6] == "UND":
            if f[4] != "WEAK": und.add(name)
        else: dfn.add(name)
    return und, dfn

def needed(path):
    if path.startswith("STUB:"): return []
    out = subprocess.run([RE, "-d", path], capture_output=True, text=True).stdout
    return [l.split("[")[1].rstrip("]") for l in out.splitlines() if "(NEEDED)" in l]

seen, missing_libs, where = {}, {}, {}
todo = list(entries)
while todo:
    p = todo.pop()
    if p in seen: continue
    seen[p] = needed(p)
    for n in seen[p]:
        q, kind = find(n)
        if q is None: missing_libs.setdefault(n, []).append(os.path.basename(p))
        else:
            where[n] = kind
            todo.append(q)

defs = {}
for p in seen:
    defs[p] = syms(p)[1]
gaps = {}
for p in seen:
    if p.startswith(S) or p.startswith("STUB:"): continue
    und = syms(p)[0]
    # symbols are global in Android's linker namespace-less M: anything in the closure counts
    for s in und:
        if not any(s in d for d in defs.values()):
            gaps.setdefault(s, []).append(os.path.relpath(p, V))
vend = sorted(os.path.relpath(p, V) for p in seen if not p.startswith(S) and not p.startswith("STUB:"))
sysl = sorted(os.path.basename(p) for p in seen if p.startswith(S))
print("vendor libs in closure: %d" % len(vend))
for v in vend: print("  " + v)
print("system libs used: %s" % " ".join(sysl))
print("\nMISSING LIBS:")
for n, by in sorted(missing_libs.items()): print("  %s  (needed by %s)" % (n, ", ".join(sorted(set(by))[:4])))
print("\nUNRESOLVED SYMBOLS: %d" % len(gaps))
for s, by in sorted(gaps.items()): print("  %-50s %s" % (s, ", ".join(sorted(set(by))[:3])))
