#!/usr/bin/env python3
"""Optional ROM test: Sonic runs through generated 68000 C on PC."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


def run(*args, env=None):
    result = subprocess.run([str(arg) for arg in args], text=True, capture_output=True, env=env)
    if result.returncode:
        raise AssertionError(f"{' '.join(map(str, args))}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def field(output, name):
    match = re.search(rf"\b{name}=([0-9a-f]+)\b", output)
    assert match, output
    return match.group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    args = parser.parse_args()
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with tempfile.TemporaryDirectory(prefix="md_sonic_") as folder:
        root = Path(folder)
        project = root / "pc"
        run(args.recompiler, "--system", "megadrive", args.rom, "--target", "pc", "-o", project)
        assert (project / "native-status.txt").read_text() == "native-c\n"
        run("cmake", "-G", "Ninja", "-S", project, "-B", project / "build")
        run("ninja", "-C", project / "build")
        player = project / "build" / "md_pc"
        title = run(player, "--interp", "--frames", "600", "--coverage", root / "title.cov",
                    "--ppm", root / "title.ppm", env=env)
        assert field(title, "frames") == "600", title
        assert field(title, "fault") == "0", title
        assert (root / "title.ppm").stat().st_size > 320 * 224 * 3
        start = root / "start.input"
        start.write_text("600 128\n602 0\n1050 8\n1150 0\n1300 64\n1310 0\n")
        gameplay = run(player, "--interp", "--frames", "1500", "--input", start,
                       "--coverage", root / "game.cov",
                       "--ppm", root / "game.ppm", env=env)
        assert field(gameplay, "frames") == "1500", gameplay
        assert field(gameplay, "fault") == "0", gameplay
        run(args.recompiler, "--system", "megadrive", args.rom, "--target", "pc",
            "--coverage", root / "title.cov", "--coverage", root / "game.cov", "-o", project)
        run("ninja", "-C", project / "build")
        native = run(player, "--frames", "1500", "--input", start, "--ppm", root / "native.ppm", env=env)
        assert field(native, "fallback_hits") == "0", native
        assert field(native, "run_hash") == field(gameplay, "run_hash"), (gameplay, native)
        assert field(native, "image_hash") == field(gameplay, "image_hash"), (gameplay, native)
        print(title.strip())
        print(gameplay.strip())
        print(native.strip())


if __name__ == "__main__":
    main()
