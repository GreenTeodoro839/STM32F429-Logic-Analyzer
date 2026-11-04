#!/usr/bin/env python3
"""Streaming throughput test for LA-F103 / LA-F429 (sigrok-pico protocol, no sigrok needed).

Starts a fixed-length stream and reports how many bytes arrived, the achieved
USB throughput and whether the device signalled an overflow ('!').

    tools/stream_bench.py /dev/la-f429 --rate 20000000 --samples 20000000 --channels 8
    tools/stream_bench.py /dev/la-f103 --rate 2000000 --samples 4000000 --channels 4

Channels are always the first N (CH0..CH(N-1)), as the sigrok driver requires.
"""
import argparse
import os
import select
import termios
import time


def open_raw(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def read(fd, timeout):
    r, _, _ = select.select([fd], [], [], timeout)
    return os.read(fd, 65536) if r else b""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device")
    ap.add_argument("--rate", type=int, default=1000000, help="sample rate in Hz (F429: <= 20 MHz, divides 120 MHz; F103: <= 4 MHz, divides 72 MHz)")
    ap.add_argument("--samples", type=int, default=2000000)
    ap.add_argument("--channels", type=int, default=8, choices=range(1, 9))
    args = ap.parse_args()

    fd = open_raw(args.device)

    def drain():
        while read(fd, 0.1):
            pass

    def cmd(s):
        os.write(fd, s.encode())
        r = read(fd, 1.0)
        if r[:1] != b"*":
            raise SystemExit(f"command {s!r} refused: {r!r}")

    os.write(fd, b"*")
    drain()
    os.write(fd, b"i\n")
    print("ID:", read(fd, 1.0).decode(errors="replace"))
    for i in range(8):
        cmd(f"D{1 if i < args.channels else 0}{i}\n")
    cmd(f"L{args.samples}\n")
    cmd(f"R{args.rate}\n")
    drain()

    os.write(fd, b"F\n")
    t0 = time.time()
    data = bytearray()
    status = "timeout"
    while True:
        chunk = read(fd, 3.0)
        if not chunk:
            break
        data += chunk
        tail = bytes(data[-24:])
        if b"!" in tail:
            status = "OVERFLOW"
            break
        if b"$" in tail and tail.endswith(b"+"):
            status = "ok"
            break
    dt = time.time() - t0
    os.close(fd)

    body = len(data) - (len(data) - data.rfind(b"$") if status == "ok" else 0)
    print(f"{status}: {args.channels} ch @ {args.rate / 1e3:g} kHz, {args.samples} samples "
          f"({args.samples / args.rate:.2f} s of signal): {body} bytes in {dt:.2f} s "
          f"-> {body / dt / 1e3:.0f} kB/s")


if __name__ == "__main__":
    main()
