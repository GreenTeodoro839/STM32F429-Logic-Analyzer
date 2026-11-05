#!/usr/bin/env python3
"""Debug helper for the LA-F429 board over a running OpenOCD (TCL port 6666).

    swd.py shot out.png        grab the LCD frame buffer (g_fb) as an image
    swd.py keys 'A3##'         inject key presses (g_input_inject), 0.25 s apart
    swd.py read SYMBOL [N]     hex dump of N bytes at an ELF symbol
    swd.py seg7                read the text on the 7-segment display

Symbols are looked up in the ELF with arm-none-eabi-nm. Start OpenOCD with
the board config from CMakeLists.txt plus -c init; the target keeps running.
"""
import os
import socket
import subprocess
import sys
import time
import zlib
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.environ.get("LA_ELF", os.path.join(HERE, "..", "build", "f429", "la_f429.elf"))
W = H = 240


class OpenOCD:
    def __init__(self, host="127.0.0.1", port=6666):
        self.s = socket.create_connection((host, port))

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("OpenOCD closed the connection")
            buf += chunk
        return buf[:-1].decode(errors="replace")

    def read(self, addr, n, width=32):
        words = []
        step = width // 8
        count = (n + step - 1) // step
        i = 0
        while i < count:  # in pieces, OpenOCD replies get slow when huge
            k = min(512, count - i)
            out = self.cmd(f"read_memory {addr + i * step:#x} {width} {k}")
            words += [int(x, 16) for x in out.split()]
            i += k
        return b"".join(w.to_bytes(step, "little") for w in words)[:n]

    def write8(self, addr, v):
        self.cmd(f"write_memory {addr:#x} 8 {{{v}}}")


def symbols():
    out = subprocess.run(["arm-none-eabi-nm", "-S", ELF], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            syms[p[2]] = (int(p[0], 16), 0)
    return syms


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def shot(ocd, syms, path, scale=2):
    addr, _ = syms["g_fb"]
    data = ocd.read(addr, W * H * 2)
    px = bytearray()
    for y in range(H):
        row = bytearray()
        for x in range(W):
            c = data[(y * W + x) * 2] | data[(y * W + x) * 2 + 1] << 8
            rgb = bytes(((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31))
            row += rgb * scale
        px += bytes(row) * scale
    write_png(path, W * scale, H * scale, bytes(px))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    ocd = OpenOCD()
    syms = symbols()
    op = sys.argv[1]
    if op == "shot":
        shot(ocd, syms, sys.argv[2] if len(sys.argv) > 2 else "screen.png")
    elif op == "keys":
        addr, _ = syms["g_input_inject"]
        for k in sys.argv[2]:
            code = {"[": 0x81, "]": 0x82}.get(k, ord(k))  # [ ] = KEY1 / KEY2 buttons
            for _ in range(50):  # wait until the previous one was taken
                if ocd.read(addr, 1, 8)[0] == 0:
                    break
                time.sleep(0.02)
            ocd.write8(addr, code)
            time.sleep(0.25)
    elif op == "read":
        addr, size = syms[sys.argv[2]]
        n = int(sys.argv[3], 0) if len(sys.argv) > 3 else size
        data = ocd.read(addr & ~3, n + (addr & 3))[addr & 3:]
        for i in range(0, len(data), 16):
            print(f"{addr + i:08x}  " + " ".join(f"{b:02x}" for b in data[i:i + 16]))
    elif op == "seg7":
        addr, size = syms["s_seg7"]
        data = ocd.read(addr & ~3, size + 4)[addr & 3:][:size]
        print(data.split(b"\0")[0].decode(errors="replace"))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
