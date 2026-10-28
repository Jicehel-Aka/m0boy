"""M0+ firmware C dispatch and interpreter agree for both flash maps."""
from __future__ import annotations

import pathlib
import os
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
            data = bytearray(80)
            struct.pack_into("<II", data, 0, ram + 0x8000, flash + 9)
            ops = (0x2005, 0x2103, 0x1842, 0x3A01, 0xAC00, 0x6022,
                   0x6825, 0x2D07, 0xD100, 0xBF00,
                   0x0040, 0x0849, 0x108A, 0x4008, 0x4048, 0x4308,
                   0x4680, 0x4646, 0xB403, 0xBC0C, 0xB084, 0xA800,
                   0x2100, 0x5041, 0x5842, 0xB004, 0xF3EF, 0x8110,
                   0xF381, 0x8810, 0xF000, 0xF800, 0xE7FE)
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
            if board == "meta":
                # No emulator: the translated entries, the CPU and the SAMD21
                # board.  The interpreter-only run must give the same line.
                assert not (project / "src" / "gm0.c").exists()
                exe = project / "build" / "m0_headless"
                native = run(exe, "--frames", 3)
                interp = run(exe, "--frames", 3, "--interp")
                assert "fault=0" in native, native
                assert re.sub(r"idle_skipped=\d+ ", "", native) == re.sub(r"idle_skipped=\d+ ", "", interp), (native, interp)
                # the standard profile: coverage a regeneration reads back
                run(exe, "--frames", 3, "--random", 1, "--profile", project / "p")
                assert (project / "p.cov").read_text().strip(), "empty coverage"
                assert (project / "p.mmio").exists() and (project / "p.hot").exists()
                run(recompiler, firmware, "--system", board, "--target", "pc",
                    "--coverage", project / "p.cov", "-o", project)
                continue
            exe = project / "build" / "m0_native_pc"
            native = run(exe, "--steps", 40)
            interp = run(exe, "--interp", "--steps", 40)
            assert "fault=0 bus_fault=0" in native, native
            assert re.search(r"native=(\d+)", native).group(1) != "0", native
            assert re.search(r"run_hash=([0-9a-f]+)", native).group(1) == re.search(
                r"run_hash=([0-9a-f]+)", interp).group(1), (native, interp)
            assert "native=0" in interp, interp
            assert int(re.search(r"board_native_entries=(\d+)", status).group(1)) > 0
            # Exercise the translated instructions in the actual board model,
            # including its scheduler and peripheral state, against its own
            # interpreter. The headless build prints CPU and screen hashes.
            run("ninja", "-C", project / "build", "m0_board_test")
            board_test = project / "build" / "m0_board_test"
            card = root / f"{board}_card"
            card.mkdir()
            def board_run(interpreted: bool) -> tuple[str, str]:
                env = os.environ.copy()
                if interpreted:
                    env["M0_INTERP"] = "1"
                else:
                    env.pop("M0_INTERP", None)
                result = subprocess.run([str(board_test), str(firmware), str(card), "2"],
                                        env=env, text=True, capture_output=True, check=True)
                final = re.search(r"FINAL (\d+ [0-9a-f]+)", result.stderr)
                screen = re.search(r"ECRAN ([0-9a-f]+ nz=\d+)", result.stderr)
                assert final and screen, result.stderr
                return final.group(1), screen.group(1)
            assert board_run(False) == board_run(True)
            # The graphical executable contains the original firmware and can
            # boot without the separate .bin, including with an explicit card.
            (project / "firmware.bin").unlink()
            env = os.environ.copy()
            env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
            result = subprocess.run([str(project / "build" / "m0_firmware_pc"),
                                     "--target", board, str(card), "--frames", "2"],
                                    env=env, text=True, capture_output=True, check=True)
            assert "firmware.bin" in result.stdout + result.stderr, result.stdout + result.stderr


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]))
