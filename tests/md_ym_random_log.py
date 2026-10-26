#!/usr/bin/env python3
"""Writes a seeded random YM2612 write log (md_pc --ym-log format) that walks
every algorithm, feedback, detune/multiple, key scaling, SSG-EG mode, LFO
AM/PM, channel 3's special mode, the DAC and key on/off, for
md_ym_compare."""
import random
import sys


def main():
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    lines = []
    sample = 100

    def write(bank, reg, value):
        lines.append(f"{sample} {bank} {reg:02x} {value:02x}\n")

    write(0, 0x2b, 0x00)
    for note in range(240):
        ch = rng.randrange(6)
        bank, lane = divmod(ch, 3)
        key = (lane | (bank << 2))
        write(0, 0x28, key)                     # key off before the new patch
        write(0, 0x22, rng.choice([0, 0, 8 | rng.randrange(8)]))
        if ch == 2:
            write(0, 0x27, rng.choice([0x00, 0x00, 0x40]))
            for reg in (0xac, 0xa8, 0xad, 0xa9, 0xae, 0xaa):
                write(0, reg, rng.randrange(0x40) if reg >= 0xac else rng.randrange(256))
        for op in range(4):
            at = lane + op * 4
            write(bank, 0x30 + at, rng.randrange(128))
            write(bank, 0x40 + at, rng.choice([0, 8, 0x18, 0x28, rng.randrange(128)]))
            write(bank, 0x50 + at, rng.randrange(256) | 0x10)
            write(bank, 0x60 + at, rng.randrange(256))
            write(bank, 0x70 + at, rng.randrange(32))
            write(bank, 0x80 + at, rng.randrange(256))
            write(bank, 0x90 + at, rng.choice([0, 0, 8 | rng.randrange(8)]))
        write(bank, 0xb0 + lane, rng.randrange(64))
        write(bank, 0xb4 + lane, rng.choice([0xc0, 0x80, 0x40]) | rng.randrange(64))
        write(bank, 0xa4 + lane, rng.randrange(0x40))
        write(bank, 0xa0 + lane, rng.randrange(256))
        write(0, 0x28, key | (rng.randrange(1, 16) << 4))
        if rng.random() < 0.15:                  # a burst of DAC samples
            write(0, 0x2b, 0x80)
            for _ in range(rng.randrange(50, 400)):
                write(0, 0x2a, rng.randrange(256))
                sample += rng.randrange(2, 6)
            write(0, 0x2b, 0x00)
        sample += rng.randrange(200, 6000)
        if rng.random() < 0.5:
            write(0, 0x28, key)
            sample += rng.randrange(100, 3000)
    with open(sys.argv[1], "w") as f:
        f.writelines(lines)


if __name__ == "__main__":
    main()
