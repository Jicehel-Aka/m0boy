#!/usr/bin/env python3
"""Debug a running META firmware over its USB serial port (META_USB, meta_usb.c).

  meta-usb.py info                          firmware's answer to "V#"
  meta-usb.py read  --elf E SYMBOL [BYTES]  memory by symbol (or 0x address)
  meta-usb.py write --elf E SYMBOL VALUE    a 32-bit word (--byte: a byte), RAM only
  meta-usb.py prof  --elf E [--seconds 5]   PC samples (SysTick, meta_usb_pc) by function
  meta-usb.py press --elf E KEYS [--ms 150] hold buttons: down,left,right,up,a,b,menu,home
  meta-usb.py fault --elf E                 the last fault (kept across the automatic
                                            restart that follows it), with addr2line
  meta-usb.py boot                          1200-baud touch: the console goes to sam-ba
  meta-usb.py flash FIRMWARE.bin            boot, then bossac at 0x4000, then reset

The firmware answers sam-ba's commands (R, W, O, V), so reads go through the
same protocol as the bootloader's.  Profiling samples the PC SysTick interrupts
every millisecond into a ring of 32 (meta_usb_prof); the host drains it
every 20 ms (about 50 USB requests a second) and maps the PCs to the ELF's
functions.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import glob
import os
import shutil
import struct
import subprocess
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required (pip install pyserial)")

VID, APP_PID, BOOT_PID = 0x2341, 0x804D, 0x004D
BUTTONS = {"down": 0x01, "left": 0x02, "right": 0x04, "up": 0x08, "a": 0x10, "b": 0x20, "menu": 0x40, "home": 0x80}


def find_port(pid: int, wait: float = 0.0) -> str | None:
    end = time.time() + wait
    while True:
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid == pid:
                return p.device
        if time.time() >= end:
            return None
        time.sleep(0.2)


class Console:
    def __init__(self, port: str | None):
        port = port or find_port(APP_PID)
        if not port:
            sys.exit("no META firmware with USB found (2341:804d)")
        self.s = serial.Serial(port, 115200, timeout=1)
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def version(self) -> str:
        self.s.write(b"V#")
        time.sleep(0.1)
        return self.s.read(self.s.in_waiting or 1).decode(errors="replace").strip()

    def read(self, addr: int, n: int) -> bytes:
        self.s.write(b"R%08X,%08X#" % (addr, n))
        data = b""
        end = time.time() + 2
        while len(data) < n and time.time() < end:
            data += self.s.read(n - len(data))
        if len(data) != n:
            raise IOError("short read at 0x%08x (%d of %d bytes)" % (addr, len(data), n))
        return data

    def write(self, addr: int, value: int, size: int = 4) -> None:
        self.s.write(b"%s%08X,%08X#" % (b"W" if size == 4 else b"O", addr, value))
        self.s.flush()


def toolchain(tool: str) -> str:
    name = "arm-none-eabi-" + tool
    found = shutil.which(name)
    if found:
        return found
    for path in glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi*/bin/" + name)):
        return path
    sys.exit(name + " not found (PATH or PlatformIO's toolchain)")


def symbols(elf: str) -> dict[str, tuple[int, int]]:
    out = subprocess.run([toolchain("nm"), "-S", "-C", elf], stdout=subprocess.PIPE, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        f = line.split(None, 3)
        if len(f) == 4:
            table[f[3]] = (int(f[0], 16), int(f[1], 16))
        elif len(f) == 3:
            table[f[2]] = (int(f[0], 16), 0)
    return table


def address(arg: str, elf: str | None) -> tuple[int, int]:
    if arg.lower().startswith("0x"):
        return int(arg, 16), 4
    if not elf:
        sys.exit("--elf is needed to use a symbol name")
    table = symbols(elf)
    if arg not in table:
        sys.exit("symbol not found: " + arg)
    return table[arg][0], table[arg][1] or 4


def functions(elf: str) -> tuple[list[int], list[str]]:
    out = subprocess.run([toolchain("nm"), "-n", "-C", elf], stdout=subprocess.PIPE, text=True, check=True).stdout
    starts, names = [], []
    for line in out.splitlines():
        f = line.split(None, 2)
        # code in flash, and the routines copied to SRAM (named in .data)
        if len(f) == 3 and (f[1] in "tTwW" or (f[1] in "dD" and int(f[0], 16) >= 0x20000000)):
            starts.append(int(f[0], 16) & ~1)
            names.append(f[2])
    return starts, names


def cmd_prof(c: Console, elf: str, seconds: float, top: int) -> None:
    ring = 32  # META_USB_PROF_SAMPLES
    prof = address("meta_usb_prof", elf)[0]
    starts, names = functions(elf)
    hist = collections.Counter()
    last, samples, lost = None, 0, 0
    end = time.time() + seconds
    while time.time() < end:
        words = struct.unpack("<%dI" % (1 + ring), c.read(prof, 4 * (1 + ring)))
        count = words[0]
        if last is not None:
            new = (count - last) & 0xFFFFFFFF
            if new > ring - 2:  # the ring went round (or nearly) since the last read
                lost += new - (ring - 2)
                new = ring - 2
            for k in range(count - new, count):
                pc = words[1 + (k & (ring - 1))]
                i = bisect.bisect_right(starts, pc & ~1) - 1
                hist[names[i] if i >= 0 else "0x%08x" % pc] += 1
                samples += 1
        last = count
        time.sleep(0.02)
    print("%d samples in %.1f s (%d lost)" % (samples, seconds, lost))
    for name, n in hist.most_common(top):
        print("%6.2f%%  %6d  %s" % (100.0 * n / max(samples, 1), n, name))


def cmd_fault(c: Console, elf: str) -> None:
    w = struct.unpack("<13I", c.read(address("meta_fault", elf)[0], 52))
    magic, count, rcause, early, frame, exception = w[0], w[1], w[2], w[3], w[4:12], w[12]
    causes = {0x01: "power-on", 0x02: "brown-out 1.2 V", 0x04: "brown-out 3.3 V", 0x10: "RESET button",
              0x20: "watchdog (a lockup: interrupts were off)", 0x40: "software (sam-ba, or a restart after a fault)"}
    print("this boot: %s" % ", ".join(n for b, n in causes.items() if rcause & b) or hex(rcause))
    if magic != 0x46415532 or not count:
        print("no fault recorded")
        return
    kind = {2: "NMI", 3: "HardFault", 18: "watchdog early warning: main loop stuck"}.get(exception, "IRQ %d" % (exception - 16))
    print("%d fault(s) logged; the last: exception %d (%s)" % (count, exception, kind))
    if early >= 3:
        print("%d in a row right after boot: restarts stopped, the firmware waits for the host" % early)
    for n, v in zip(["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr"], frame):
        print("%-5s 0x%08x" % (n, v))
    out = subprocess.run([toolchain("addr2line"), "-f", "-C", "-e", elf, "0x%x" % frame[6], "0x%x" % frame[5]],
                         stdout=subprocess.PIPE, text=True).stdout.split("\n")
    print("pc in %s (%s)" % (out[0], out[1]))
    print("lr in %s (%s)" % (out[2], out[3]))


def cmd_boot(port: str | None) -> None:
    port = port or find_port(APP_PID)
    if not port:
        sys.exit("no META firmware with USB found (2341:804d)")
    s = serial.Serial(port, 1200)
    time.sleep(0.1)
    s.close()
    boot = find_port(BOOT_PID, wait=10)
    if not boot:
        sys.exit("the bootloader did not show up")
    time.sleep(2)  # sam-ba's CDC ignores early requests
    print(boot)


def cmd_flash(port: str | None, firmware: str) -> None:
    boot = find_port(BOOT_PID)
    if not boot:
        cmd_boot(port)
        boot = find_port(BOOT_PID, wait=10)
    bossac = shutil.which("bossac") or os.path.expanduser("~/.platformio/packages/tool-bossac/bossac")
    subprocess.run([bossac, "-p", boot, "-o", "0x4000", "-e", "-w", "-v", firmware, "-R"], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    r = sub.add_parser("read")
    r.add_argument("--elf")
    r.add_argument("what")
    r.add_argument("bytes", nargs="?", type=int)
    w = sub.add_parser("write")
    w.add_argument("--elf")
    w.add_argument("--byte", action="store_true")
    w.add_argument("what")
    w.add_argument("value", type=lambda v: int(v, 0))
    p = sub.add_parser("prof")
    p.add_argument("--elf", required=True)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--top", type=int, default=25)
    k = sub.add_parser("press")
    k.add_argument("--elf", required=True)
    k.add_argument("--ms", type=int, default=150)
    k.add_argument("keys", help="comma-separated: " + ",".join(BUTTONS))
    q = sub.add_parser("fault")
    q.add_argument("--elf", required=True)
    sub.add_parser("boot")
    f = sub.add_parser("flash")
    f.add_argument("firmware")
    a = ap.parse_args()

    if a.cmd == "boot":
        cmd_boot(a.port)
        return 0
    if a.cmd == "flash":
        cmd_flash(a.port, a.firmware)
        return 0
    c = Console(a.port)
    if a.cmd == "info":
        print(c.version())
    elif a.cmd == "read":
        addr, size = address(a.what, a.elf)
        data = c.read(addr, a.bytes or size)
        if len(data) in (1, 2, 4) and not a.bytes:
            v = int.from_bytes(data, "little")
            print("0x%08x: %d (0x%x)" % (addr, v, v))
        else:
            for i in range(0, len(data), 16):
                print("%08x  %s" % (addr + i, data[i:i + 16].hex(" ")))
    elif a.cmd == "write":
        addr, _ = address(a.what, a.elf)
        c.write(addr, a.value, 1 if a.byte else 4)
    elif a.cmd == "prof":
        cmd_prof(c, a.elf, a.seconds, a.top)
    elif a.cmd == "fault":
        cmd_fault(c, a.elf)
    elif a.cmd == "press":
        bits = 0
        for key in a.keys.lower().split(","):
            if key not in BUTTONS:
                sys.exit("unknown key: " + key)
            bits |= BUTTONS[key]
        addr, _ = address("meta_usb_buttons", a.elf)
        c.write(addr, bits, 1)
        time.sleep(a.ms / 1000.0)
        c.write(addr, 0, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
