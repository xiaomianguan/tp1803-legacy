#!/usr/bin/env python3
"""Bake an unpacked OpenGApps 6.0 arm64 nano package into a target-files zip.

usage: add-gapps.py <target_files.zip> <opengapps tars dir> <g.prop> <out.zip>

<opengapps tars dir> holds the extracted Core/GApps/Optional tarballs
(one folder per package, e.g. gmscore-arm64/nodpi/priv-app/...). The
package selection mirrors what the OpenGApps installer does for nano on a
non-CM (AOSPA) phone at 420 dpi:
  - all nano packages except setupwizardtablet (phone); Google's
    PackageInstaller replaces the AOSP one (the installer only skips it on CM)
  - GmsCore: no 420 build, so the installer's rule picks nodpi
  - Provision removed (conflicts with Google SetupWizard)
  - swypelibs + LatinIME symlinks so the AOSP keyboard gets gesture typing
IMAGES/system.img is dropped so ota_from_target_files rebuilds it.
"""
import os, sys, zipfile

PACKAGES = [
    "configupdater-all/nodpi", "defaultetc-common/common", "defaultframework-common/common",
    "gmscore-arm64/nodpi", "googlebackuptransport-all/nodpi", "googlecontactssync-all/nodpi",
    "googlefeedback-all/nodpi", "googleonetimeinitializer-all/nodpi",
    "googlepartnersetup-all/nodpi", "gsfcore-all/nodpi", "gsflogin-all/nodpi",
    "setupwizarddefault-all/nodpi", "vending-arm64/nodpi",
    "calsync-all/nodpi", "dialerframework-common/common", "googletts-arm64/nodpi",
    "search-arm64/nodpi", "speech-common/common", "swypelibs-lib-arm64/common",
    "packageinstallergoogle-all/nodpi",
]
REMOVE = ("system/app/Provision", "system/priv-app/PackageInstaller")
SYMLINKS = {  # LatinIME uses the Google gesture-typing library
    "system/app/LatinIME/lib/arm64/libjni_latinime.so": "/system/lib64/libjni_latinimegoogle.so",
    "system/app/LatinIME/lib/arm64/libjni_keyboarddecoder.so": "/system/lib64/libjni_keyboarddecoder.so",
}
SEL = "selabel=u:object_r:system_file:s0 capabilities=0x0"

src, tars, gprop, dst = sys.argv[1:]

# path under system/ -> host file
files = {}
for pkg in PACKAGES:
    root = os.path.join(tars, pkg)
    assert os.path.isdir(root), root
    for d, _, names in os.walk(root):
        for n in names:
            full = os.path.join(d, n)
            files["system/" + os.path.relpath(full, root)] = full
files["system/etc/g.prop"] = gprop

def removed(path):
    return any(path == r or path.startswith(r + "/") for r in REMOVE)

with zipfile.ZipFile(src) as zi:
    fs_config = zi.read("META/filesystem_config.txt").decode().splitlines()
    existing = {l.split()[0] for l in fs_config if l.strip()}
    clash = [p for p in files if p in existing]
    assert not clash, "GApps would overwrite ROM files: %s" % clash

    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as zo:
        for info in zi.infolist():
            name = info.filename
            if name in ("IMAGES/system.img", "IMAGES/system.map"):
                continue
            if name.startswith("SYSTEM/") and removed("system/" + name[7:].rstrip("/")):
                continue
            if name.startswith("SYSTEM/") and "system/" + name[7:] in SYMLINKS:
                continue  # replaced below
            if name == "META/filesystem_config.txt":
                continue
            with zi.open(info) as f, zo.open(info, "w", force_zip64=True) as g:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    g.write(b)

        new_lines = []
        dirs = set()
        for p in list(files) + list(SYMLINKS):
            parts = p.split("/")
            for i in range(2, len(parts)):
                dirs.add("/".join(parts[:i]))
        for d in sorted(dirs):
            if d not in existing:
                zo.writestr(zipfile.ZipInfo("SYSTEM/" + d[7:] + "/"), b"")
                new_lines.append("%s 0 0 755 %s" % (d, SEL))
        for p, host in sorted(files.items()):
            zo.write(host, "SYSTEM/" + p[7:])
            new_lines.append("%s 0 0 644 %s" % (p, SEL))
        for p, target in sorted(SYMLINKS.items()):
            zi_ = zipfile.ZipInfo("SYSTEM/" + p[7:])
            zi_.create_system = 3
            zi_.external_attr = (0o120777 << 16)
            zo.writestr(zi_, target)
            if p not in existing:
                new_lines.append("%s 0 0 644 %s" % (p, SEL))

        kept = [l for l in fs_config if l.strip() and not removed(l.split()[0])]
        zo.writestr("META/filesystem_config.txt", "\n".join(sorted(kept + new_lines)) + "\n")

print("added %d files, %d dirs, %d symlinks; removed %s" %
      (len(files), len([l for l in new_lines if " 755 " in l]), len(SYMLINKS), ", ".join(REMOVE)))
