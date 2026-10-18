#!/usr/bin/env python3
"""Drives the Xbox Bridge from a Mac or Linux machine. Standard library only.

usage: xbox_pad.py demo          press A, then circle the left stick
       xbox_pad.py press <name>  press one button (a b x y lb rb lt rt view menu xbox l3 r3 share up down left right)
       xbox_pad.py neutral       release everything

Open "Xbox Bridge" on the Flipper first. Pair "Xbox Wireless Controller" from the iPhone's Bluetooth
settings the first time; after that it reconnects by itself.
"""
import glob, math, os, select, struct, sys, termios, time

BUTTON_BITS = {"a": 0, "b": 1, "x": 3, "y": 4, "lb": 6, "rb": 7, "view": 10, "menu": 11, "xbox": 12, "l3": 13, "r3": 14}
HAT = {"up": 1, "right": 3, "down": 5, "left": 7}


def report(lx=0.0, ly=0.0, rx=0.0, ry=0.0, lt=0.0, rt=0.0, buttons=(), hat=0, share=False):
    """Input report 1 without its ID byte: sticks -1..1 (y down), triggers 0..1, hat 1-8 clockwise from up."""
    axis = lambda v: round((max(-1.0, min(1.0, v)) + 1) / 2 * 0xFFFF)
    trigger = lambda v: round(max(0.0, min(1.0, v)) * 0x3FF)
    bits = sum(1 << BUTTON_BITS[b] for b in buttons)
    return struct.pack("<6HBHB", axis(lx), axis(ly), axis(rx), axis(ry), trigger(lt), trigger(rt), hat, bits, int(share))


class Bridge:
    """Frames on the Flipper's second USB serial port: 0xA5, type, length, payload."""

    def __init__(self):
        for port in sorted(glob.glob("/dev/cu.usbmodemflip_*") + glob.glob("/dev/ttyACM*"), reverse=True):
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(fd)
            a[0] = a[1] = a[3] = 0
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            termios.tcsetattr(fd, termios.TCSANOW, a)
            self.fd, self.buf = fd, b""
            self.send(0x03)  # status
            if self.wait(0x81, 0.6) is not None:
                return
            os.close(fd)
        raise SystemExit("Bridge not found. Open Xbox Bridge on the Flipper.")

    def send(self, kind, payload=b""):
        os.write(self.fd, bytes([0xA5, kind, len(payload)]) + bytes(payload))

    def wait(self, kind, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if select.select([self.fd], [], [], 0.05)[0]:
                try:
                    self.buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass
            while (i := self.buf.find(b"\xA5")) >= 0 and len(self.buf) - i >= 3 and len(self.buf) - i >= 3 + self.buf[i + 2]:
                n = self.buf[i + 2]
                t, p = self.buf[i + 1], self.buf[i + 3 : i + 3 + n]
                self.buf = self.buf[i + 3 + n :]
                if t == kind:
                    return p
        return None

    def pad(self, data):
        self.send(0x10, data)


def main():
    cmd = sys.argv[1:] or ["demo"]
    b = Bridge()
    if cmd[0] == "neutral":
        b.pad(report())
    elif cmd[0] == "press" and len(cmd) == 2:
        name = cmd[1].lower()
        b.pad(report(buttons=[name] if name in BUTTON_BITS else (), hat=HAT.get(name, 0), share=name == "share",
                     lt=name == "lt", rt=name == "rt"))
        time.sleep(0.15)
        b.pad(report())
    elif cmd[0] == "demo":
        b.pad(report(buttons=["a"]))
        time.sleep(0.15)
        for i in range(240):  # two circles at 120 Hz
            t = i / 120 * 2 * math.pi
            b.pad(report(lx=math.cos(t), ly=math.sin(t)))
            time.sleep(1 / 120)
        b.pad(report())
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
