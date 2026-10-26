#!/usr/bin/env python3
"""The Mega Drive YM2612 (md_ym2612.c) gives ymfm's samples exactly, on seeded
random write logs that walk every algorithm, SSG-EG mode, the LFO, channel 3's
special mode and the DAC (tests/md_ym_random_log.py)."""
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    compare = sys.argv[1]
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="md_ym_") as folder:
        for seed in (1, 2, 3):
            log = Path(folder) / f"random{seed}.log"
            subprocess.run([sys.executable, str(here / "md_ym_random_log.py"), str(log), str(seed)], check=True)
            result = subprocess.run([compare, str(log), "--exact"], text=True, capture_output=True)
            print(result.stdout.strip())
            if result.returncode:
                raise SystemExit(f"seed {seed}: YM2612 differs from ymfm\n{result.stderr}")


if __name__ == "__main__":
    main()
