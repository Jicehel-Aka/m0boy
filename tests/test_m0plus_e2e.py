"""M0+ firmware C dispatch and interpreter agree for both flash maps."""
from __future__ import annotations

import pathlib
import re
import struct
import subprocess
import sys
import tempfile


def run(*args: object) -> str:
    return subprocess.check_output([str(arg) for arg in args], text=True)


def main(recompiler: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="m0boy_m0plus_") as temporary:
        root = pathlib.Path(temporary)
        for board, flash, ram in (("meta", 0x4000, 0x20000000),
                                  ("pokitto", 0, 0x10000000)):
            firmware = root / f"{board}.bin"
            data = bytearray(40)
            struct.pack_into("<II", data, 0, ram + 0x8000, flash + 9)
            ops = (0x2005, 0x2103, 0x1842, 0x3A01, 0xAC00, 0x6022,
                   0x6825, 0x2D07, 0xD001, 0x2063, 0xE7FE, 0xBF00,
                   0xE7FE, 0xBF00, 0xE7FE)
            struct.pack_into("<" + "H" * len(ops), data, 8, *ops)
            firmware.write_bytes(data)
            coverage = root / f"{board}.cov"
            coverage.write_text(f"{flash + 34:x} 12\n")
            project = root / f"{board}_pc"
            run(recompiler, firmware, "--system", board, "--target", "pc",
                "--coverage", coverage, "-o", project)
            status = (project / "native-status.txt").read_text()
            count = int(re.search(r"compiled_flash_entries=(\d+)", status).group(1))
            assert count >= 14, status
            run("cmake", "-G", "Ninja", "-S", project, "-B", project / "build")
            run("ninja", "-C", project / "build")
            exe = project / "build" / "m0_native_pc"
            native = run(exe, "--steps", 40)
            interp = run(exe, "--interp", "--steps", 40)
            assert "fault=0 bus_fault=0" in native, native
            assert re.search(r"native=(\d+)", native).group(1) != "0", native
            assert re.search(r"run_hash=([0-9a-f]+)", native).group(1) == re.search(
                r"run_hash=([0-9a-f]+)", interp).group(1), (native, interp)
            assert "native=0" in interp, interp


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]))
