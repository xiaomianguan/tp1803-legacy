#!/usr/bin/env python3
"""Patch files inside a newc cpio archive without extracting (keeps uid/gid/modes).
usage: cpio_patch.py in.cpio out.cpio name=@hostfile | name+=text | name:s/old/new ...
"""
import sys

def parse(data):
    off, ents = 0, []
    while True:
        h = data[off:off+110]
        assert h[:6] in (b'070701', b'070702'), h[:6]
        f = [int(h[6+i*8:14+i*8], 16) for i in range(13)]
        namesize, filesize = f[11], f[6]
        name = data[off+110:off+110+namesize-1].decode()
        p = off + 110 + namesize
        p = (p + 3) & ~3
        body = data[p:p+filesize]
        off = (p + filesize + 3) & ~3
        ents.append([h[:6], f, name, body])
        if name == 'TRAILER!!!':
            return ents

def build(ents):
    out = bytearray()
    for magic, f, name, body in ents:
        f = list(f); f[6] = len(body); f[11] = len(name) + 1
        out += magic + b''.join(b'%08X' % v for v in f) + name.encode() + b'\0'
        while len(out) % 4: out += b'\0'
        out += body
        while len(out) % 4: out += b'\0'
    return bytes(out)

ents = parse(open(sys.argv[1], 'rb').read())
byname = {e[2]: e for e in ents}
for op in sys.argv[3:]:
    if '+=' in op:
        n, t = op.split('+=', 1); byname[n][3] += t.encode().decode('unicode_escape').encode()
    elif '=@' in op:
        n, p = op.split('=@', 1); byname[n][3] = open(p, 'rb').read()
    elif ':s/' in op:
        n, r = op.split(':s/', 1); a, b = r.rstrip('/').split('/', 1)
        assert a.encode() in byname[n][3], (n, a)
        byname[n][3] = byname[n][3].replace(a.encode(), b.encode())
    print('patched', op.split('=')[0].split(':')[0].rstrip('+'))
open(sys.argv[2], 'wb').write(build(ents))
