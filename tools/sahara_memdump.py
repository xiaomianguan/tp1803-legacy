#!/usr/bin/env python3
"""
Read RAM from a Qualcomm device in crashdump mode (USB 05c6:900e) using the
Sahara memory-debug protocol, and pull the kernel log out of it.

  sahara_memdump.py list                 list the dump regions
  sahara_memdump.py scan [PATTERN...]    stream DDR, print text around patterns
  sahara_memdump.py dump NAME OUT        save one region to a file
"""
import re
import struct
import sys

import usb.core
import usb.util

HELLO, HELLO_RESP, MEM_DEBUG, MEM_READ, RESET = 0x1, 0x2, 0x9, 0xA, 0x7
MEM_DEBUG64, MEM_READ64 = 0x10, 0x11
CHUNK = 1 << 20


class Sahara:
    def __init__(self):
        self.dev = usb.core.find(idVendor=0x05C6, idProduct=0x900E)
        if self.dev is None:
            sys.exit("no device in crashdump mode (05c6:900e)")
        if self.dev.is_kernel_driver_active(0):
            self.dev.detach_kernel_driver(0)
        usb.util.claim_interface(self.dev, 0)
        self.ep_in, self.ep_out = 0x81, 0x01

    def restart(self):
        """The target only sends HELLO once per session; a USB port reset
        makes the crashdump firmware start a new Sahara session."""
        try:
            self.dev.reset()
        except usb.core.USBError:
            pass
        import time
        time.sleep(2)
        self.__init__()

    def rx(self, n=4096, timeout=5000):
        return bytes(self.dev.read(self.ep_in, n, timeout))

    def tx(self, data):
        self.dev.write(self.ep_out, data, 5000)

    def hello(self):
        pkt = b""
        for attempt in range(4):
            try:
                pkt = self.rx(timeout=3000)
                while len(pkt) < 8:          # skip zero-length packets
                    pkt = self.rx(timeout=3000)
                break
            except usb.core.USBTimeoutError:
                print("no HELLO; resetting USB to restart the session", file=sys.stderr)
                self.restart()
        if len(pkt) < 8:
            sys.exit("device never sent HELLO")
        cmd, ln = struct.unpack_from("<II", pkt)
        if cmd != HELLO:
            sys.exit("expected HELLO, got cmd 0x%x" % cmd)
        version, compat, maxlen, mode = struct.unpack_from("<IIII", pkt, 8)
        print("HELLO version %d compat %d mode %d" % (version, compat, mode), file=sys.stderr)
        self.tx(struct.pack("<12I", HELLO_RESP, 0x30, 2, 1, 0, mode, 0, 0, 0, 0, 0, 0))
        pkt = self.rx()
        while len(pkt) < 8:
            pkt = self.rx()
        cmd = struct.unpack_from("<I", pkt)[0]
        if cmd == MEM_DEBUG64:
            self.is64 = True
            self.table_addr, self.table_len = struct.unpack_from("<QQ", pkt, 8)
        elif cmd == MEM_DEBUG:
            self.is64 = False
            self.table_addr, self.table_len = struct.unpack_from("<II", pkt, 8)
        else:
            sys.exit("expected MEMORY_DEBUG, got cmd 0x%x" % cmd)

    def read(self, addr, length):
        if self.is64:
            self.tx(struct.pack("<IIQQ", MEM_READ64, 24, addr, length))
        else:
            self.tx(struct.pack("<IIII", MEM_READ, 16, addr, length))
        out = bytearray()
        while len(out) < length:
            out += self.rx(min(length - len(out), CHUNK), timeout=20000)
        # A transfer that ends on a packet boundary is terminated by a
        # zero-length packet; it must be consumed or the next request stalls.
        try:
            self.rx(512, timeout=50)
        except usb.core.USBTimeoutError:
            pass
        return bytes(out)

    def regions(self):
        raw = self.read(self.table_addr, self.table_len)
        regs = []
        size = 64 if self.is64 else 52
        for i in range(0, len(raw) - size + 1, size):
            if self.is64:
                pref, base, ln = struct.unpack_from("<QQQ", raw, i)
                desc, fname = raw[i + 24:i + 44], raw[i + 44:i + 64]
            else:
                pref, base, ln = struct.unpack_from("<III", raw, i)
                desc, fname = raw[i + 12:i + 32], raw[i + 32:i + 52]
            regs.append((desc.split(b"\0")[0].decode(errors="replace"),
                         fname.split(b"\0")[0].decode(errors="replace"), base, ln))
        return regs


def main():
    s = Sahara()
    s.hello()
    regs = s.regions()
    what = sys.argv[1] if len(sys.argv) > 1 else "list"
    if what == "list":
        for d, f, b, l in regs:
            print("%-20s %-20s base 0x%010x len 0x%x (%d MiB)" % (d, f, b, l, l >> 20))
    elif what == "reset":
        try:
            s.rx(512, timeout=200)       # drain any pending ZLP first
        except usb.core.USBTimeoutError:
            pass
        s.tx(struct.pack("<II", RESET, 8))
        resp = s.rx(64, timeout=3000)    # RESET_RESP; the device then reboots
        print("reset acknowledged" if struct.unpack_from("<I", resp)[0] == 8 else "reset sent")
    elif what == "dump":
        name, out = sys.argv[2], sys.argv[3]
        for d, f, b, l in regs:
            if name in (d, f):
                with open(out, "wb") as fh:
                    for off in range(0, l, CHUNK):
                        fh.write(s.read(b + off, min(CHUNK, l - off)))
                print("saved", out)
    elif what == "scan":
        pats = [p.encode() for p in (sys.argv[2:] or ["Kernel panic", "Unable to handle", "Internal error", "BUG:"])]
        rx = re.compile(b"|".join(re.escape(p) for p in pats))
        seen = set()
        for d, f, b, l in regs:
            if not f.upper().startswith("DDRCS"):
                continue
            print("scanning %s (%d MiB)" % (d, l >> 20), file=sys.stderr)
            tail = b""
            for off in range(0, l, CHUNK):
                try:
                    data = s.read(b + off, min(CHUNK, l - off))
                except usb.core.USBTimeoutError:
                    print("timeout at 0x%x; restarting session" % (b + off), file=sys.stderr)
                    s.restart(); s.hello()
                    data = s.read(b + off, min(CHUNK, l - off))
                buf = tail + data
                for m in rx.finditer(buf):
                    ctx = buf[max(0, m.start() - 6000):m.start() + 4000]
                    key = ctx[5900:6100]
                    if key in seen:
                        continue
                    seen.add(key)
                    text = re.sub(rb"[^\x09\x0a\x20-\x7e]+", b"\n", ctx).decode()
                    print("===== %s @0x%x =====\n%s" % (d, b + off + m.start() - len(tail), text))
                tail = buf[-8192:]


if __name__ == "__main__":
    main()
