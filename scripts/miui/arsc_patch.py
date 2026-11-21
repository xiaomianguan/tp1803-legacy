#!/usr/bin/env python3
"""Replace strings in an APK's resources.arsc global string pool.

usage: arsc_patch.py <in.apk> <out.apk> <old>=<new> [...]

Used on MIUI's framework-res.apk. Android M verifies only the manifest of
system APKs, so editing resources.arsc keeps the Xiaomi platform signature
valid. Strings are matched exactly and every resource that references them
changes. The output must be zipaligned afterwards (resources.arsc is stored
uncompressed and mmap()ed).
"""
import struct, sys, zipfile

RES_TABLE_TYPE, RES_STRING_POOL_TYPE = 0x0002, 0x0001
UTF8_FLAG = 1 << 8


def enc_len8(n):
    return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])


def dec_len8(b, o):
    n = b[o]
    return ((n & 0x7F) << 8 | b[o + 1], o + 2) if n & 0x80 else (n, o + 1)


def enc_len16(n):
    return struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)


def dec_len16(b, o):
    n = struct.unpack_from("<H", b, o)[0]
    if n & 0x8000:
        return ((n & 0x7FFF) << 16 | struct.unpack_from("<H", b, o + 2)[0], o + 4)
    return n, o + 2


def patch_arsc(arsc, repl):
    ttype, thsize, tsize, npkg = struct.unpack_from("<HHII", arsc, 0)
    assert ttype == RES_TABLE_TYPE
    pool_off = thsize
    ptype, phsize, psize, nstr, nsty, flags, sstart, ystart = struct.unpack_from("<HHIIIIII", arsc, pool_off)
    assert ptype == RES_STRING_POOL_TYPE
    utf8 = bool(flags & UTF8_FLAG)
    offs = struct.unpack_from("<%dI" % nstr, arsc, pool_off + phsize)
    base = pool_off + sstart
    strings = []
    for o in offs:
        p = base + o
        if utf8:
            _, p = dec_len8(arsc, p)
            n, p = dec_len8(arsc, p)
            strings.append(arsc[p:p + n].decode("utf-8"))
        else:
            n, p = dec_len16(arsc, p)
            strings.append(arsc[p:p + 2 * n].decode("utf-16-le"))
    hits = {}
    for i, s in enumerate(strings):
        if s in repl:
            strings[i] = repl[s]
            hits[s] = hits.get(s, 0) + 1
    missing = set(repl) - set(hits)
    if missing:
        sys.exit("arsc_patch: string(s) not found: %s" % ", ".join(sorted(missing)))
    data, new_offs = bytearray(), []
    for s in strings:
        new_offs.append(len(data))
        if utf8:
            e = s.encode("utf-8")
            data += enc_len8(len(s)) + enc_len8(len(e)) + e + b"\0"
        else:
            e = s.encode("utf-16-le")
            data += enc_len16(len(e) // 2) + e + b"\0\0"
    while len(data) % 4:
        data += b"\0"
    styles = b""
    if nsty:
        sty_offs_end = pool_off + phsize + 4 * nstr + 4 * nsty
        styles = arsc[pool_off + ystart:pool_off + psize]
        sty_offs = arsc[pool_off + phsize + 4 * nstr:sty_offs_end]
    else:
        sty_offs = b""
    new_sstart = phsize + 4 * nstr + 4 * nsty
    new_ystart = new_sstart + len(data) if nsty else 0
    new_psize = new_sstart + len(data) + len(styles)
    pool = struct.pack("<HHIIIIII", ptype, phsize, new_psize, nstr, nsty, flags, new_sstart, new_ystart)
    pool += arsc[pool_off + 28:pool_off + phsize]
    pool += struct.pack("<%dI" % nstr, *new_offs) + sty_offs + bytes(data) + styles
    rest = arsc[pool_off + psize:]
    out = bytearray(arsc[:pool_off]) + pool + rest
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out), hits


def main():
    src, dst = sys.argv[1:3]
    repl = dict(a.split("=", 1) for a in sys.argv[3:])
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w") as zo:
        for info in zi.infolist():
            data = zi.read(info)
            if info.filename == "resources.arsc":
                data, hits = patch_arsc(data, repl)
                for k, v in hits.items():
                    print("resources.arsc: %r -> %r (%d)" % (k, repl[k], v))
            zo.writestr(info, data, compress_type=info.compress_type)


if __name__ == "__main__":
    main()
