#!/usr/bin/env python3
"""Resolve vendor blobs' dependency closure against CM13's /system and list
libraries and symbols Android 6 can't provide.
usage: abi-gap.py <lib|lib64> <vendor root> <cm13 system root> [--prefer-vendor a.so,b.so] entry.so..."""
import os, subprocess, sys
RE = os.path.expanduser("~/cm13-build/gpu/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf")
args = sys.argv[1:]
abi, V, S = args[0], args[1], args[2]
rest = args[3:]
prefer = set()
if rest and rest[0] == "--prefer-vendor":
    prefer = set(rest[1].split(",")); rest = rest[2:]
entries = [os.path.join(V, abi, e) if not os.path.isabs(e) else e for e in rest]
VDIRS = [os.path.join(V, abi), os.path.join(V, abi, "hw")]
SDIRS = [os.path.join(S, abi), os.path.join(S, "vendor", abi)]

def find(name):
    order = ((VDIRS, "vendor"), (SDIRS, "system")) if name in prefer else ((SDIRS, "system"), (VDIRS, "vendor"))
    for dirs, kind in order:
        for d in dirs:
            p = os.path.join(d, name)
            if os.path.exists(p): return p, kind
    return None, None

def syms(path):
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
    out = subprocess.run([RE, "-d", path], capture_output=True, text=True).stdout
    return [l.split("[")[1].rstrip("]") for l in out.splitlines() if "(NEEDED)" in l]

seen, missing, todo = {}, {}, list(entries)
while todo:
    p = todo.pop()
    if p in seen: continue
    seen[p] = needed(p)
    for n in seen[p]:
        q, kind = find(n)
        if q is None: missing.setdefault(n, []).append(os.path.basename(p))
        else: todo.append(q)
defs = {p: syms(p)[1] for p in seen}
gaps = {}
for p in seen:
    if p.startswith(S): continue
    for s in syms(p)[0]:
        if not any(s in d for d in defs.values()):
            gaps.setdefault(s, []).append(os.path.basename(p))
print("vendor libs: " + " ".join(sorted(os.path.basename(p) for p in seen if not p.startswith(S))))
print("system libs: " + " ".join(sorted(os.path.basename(p) for p in seen if p.startswith(S))))
print("MISSING LIBS:")
for n, by in sorted(missing.items()): print("  %s (needed by %s)" % (n, ", ".join(sorted(set(by)))))
print("UNRESOLVED SYMBOLS: %d" % len(gaps))
for s, by in sorted(gaps.items()): print("  %-50s %s" % (s, ", ".join(sorted(set(by))[:3])))
