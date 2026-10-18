#!/usr/bin/env python3
"""FAT lookup used by the META's ROM streaming (targets/meta/platform/src/meta_fat.c).

Builds real FAT16 and FAT32 images with mkfs.fat and mtools, with and
without a partition table, the ROM in the root or in a subdirectory, with a
lower-case name, and fragmented, then checks with fat_check that the file
read back through the extent list is byte-identical.  Skips (exit 0) when
the tools are missing.
"""
from __future__ import annotations

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def sh(*cmd: str) -> None:
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_volume(path: Path, fat: int, mb: int) -> None:
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    sh("mkfs.fat", "-F", str(fat), "-S", "512", str(path))


def with_mbr(volume: Path, disk: Path, ptype: int) -> None:
    data = volume.read_bytes()
    start = 2048
    mbr = bytearray(512)
    entry = struct.pack("<B3sB3sII", 0, b"\0\0\0", ptype, b"\0\0\0", start, len(data) // 512)
    mbr[0x1BE:0x1BE + 16] = entry
    mbr[510:512] = b"\x55\xAA"
    disk.write_bytes(bytes(mbr) + bytes(512 * (start - 1)) + data)


def free_bytes(image: Path) -> int:
    out = subprocess.run(["mdir", "-i", str(image), "::/"], stdout=subprocess.PIPE, text=True).stdout
    for line in out.splitlines():
        if "bytes free" in line:
            return int("".join(ch for ch in line.split("bytes free")[0] if ch.isdigit()))
    raise SystemExit("cannot read free space:\n" + out)


def check(fat_check: str, image: Path, name: str, expected: Path, max_ext: int = 16) -> str:
    out = subprocess.run([fat_check, str(image), name, str(expected), str(max_ext)],
                         stdout=subprocess.PIPE, text=True)
    if out.returncode != 0:
        raise SystemExit("%s %s: %s" % (image.name, name, out.stdout.strip()))
    return out.stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fat-check", required=True)
    args = ap.parse_args()
    for tool in ("mkfs.fat", "mcopy", "mmd", "mdel", "mdir"):
        if not shutil.which(tool):
            print("skipped: %s not found" % tool)
            return 0
    env_ok = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    os.environ.update(env_ok)
    tmp = Path(tempfile.mkdtemp(prefix="meta_fat_"))
    try:
        rng = random.Random(7)
        rom = tmp / "mq.gb"
        rom.write_bytes(bytes(rng.randrange(256) for _ in range(256 * 1024)))
        small = tmp / "pad.bin"
        small.write_bytes(bytes(rng.randrange(256) for _ in range(20 * 1024)))

        results = []
        # FAT16 without partition table, ROM in the root.
        v16 = tmp / "fat16.img"
        make_volume(v16, 16, 32)
        sh("mcopy", "-i", str(v16), str(rom), "::/MQ.GB")
        results.append(check(args.fat_check, v16, "MQ      GB ", rom))

        # FAT32 behind an MBR, lower-case name in a subdirectory, fragmented:
        # the file is copied into a hole left by a deleted file.
        v32 = tmp / "fat32.vol"
        make_volume(v32, 32, 64)
        sh("mmd", "-i", str(v32), "::/games")
        sh("mmd", "-i", str(v32), "::/other")
        for n in range(8):
            sh("mcopy", "-i", str(v32), str(small), "::/games/pad%d.bin" % n)
        # Fill the rest of the volume except about 200 KiB, then punch holes:
        # the ROM can only fit across them.
        free = free_bytes(v32)
        filler = tmp / "filler.bin"
        with open(filler, "wb") as f:
            f.truncate(max(free - 200 * 1024, 0))
        sh("mcopy", "-i", str(v32), str(filler), "::/other/filler.bin")
        for n in (1, 3, 5):
            sh("mdel", "-i", str(v32), "::/games/pad%d.bin" % n)
        sh("mcopy", "-i", str(v32), str(rom), "::/games/mq.gb")
        disk = tmp / "fat32.img"
        with_mbr(v32, disk, 0x0C)
        out = check(args.fat_check, disk, "MQ      GB ", rom)
        results.append(out)
        if "extents 1 " in out:
            raise SystemExit("expected a fragmented file on the FAT32 image: " + out)
        # Too many runs for the caller's table: reported, not misread.
        frag = subprocess.run([args.fat_check, str(disk), "MQ      GB ", "-", "1"],
                              stdout=subprocess.PIPE, text=True).stdout
        if "result -4" not in frag:
            raise SystemExit("fragmentation not reported: " + frag)
        missing = subprocess.run([args.fat_check, str(disk), "NOPE    GB ", "-"],
                                 stdout=subprocess.PIPE, text=True).stdout
        if "result -3" not in missing:
            raise SystemExit("missing file not reported: " + missing)

        # One folder per game, each with ROM.GG and ROM.SAV (compile.sh's
        # layout): the lookup must take the ROM whose first sector matches,
        # not the first ROM.GG, and the save from the same folder.
        vg = tmp / "games.img"
        make_volume(vg, 16, 32)
        for game in ("Aladdin 48", "Ristar 48", "Sonic 48"):
            data = tmp / (game + ".gg")
            data.write_bytes(bytes(rng.randrange(256) for _ in range(64 * 1024)))
            save = tmp / (game + ".sav")
            save.write_bytes(bytes(rng.randrange(256) for _ in range(8 * 1024)))
            sh("mmd", "-i", str(vg), "::/" + game)
            sh("mcopy", "-i", str(vg), str(data), "::/%s/ROM.GG" % game)
            sh("mcopy", "-i", str(vg), str(save), "::/%s/ROM.SAV" % game)
        out = subprocess.run([args.fat_check, str(vg), "ROM     GG ", str(tmp / "Ristar 48.gg"), "16", "match",
                              "ROM     SAV", str(tmp / "Ristar 48.sav")], stdout=subprocess.PIPE, text=True)
        if out.returncode != 0:
            raise SystemExit("per-game folders: " + out.stdout.strip())
        results.append("folders " + out.stdout.strip())

        # More game folders than one batch of the lookup (MAX_SUBDIRS, 32):
        # the ROM in the 70th folder is still found.
        vm = tmp / "many.img"
        make_volume(vm, 32, 64)
        for n in range(70):
            sh("mmd", "-i", str(vm), "::/game%02d" % n)
        sh("mcopy", "-i", str(vm), str(small), "::/game69/LAST.GB")
        results.append("70 folders " + check(args.fat_check, vm, "LAST    GB ", small))
        print("meta FAT lookup:", "; ".join(results))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
