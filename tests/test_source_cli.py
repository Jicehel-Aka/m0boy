"""Synthetic cartridges only; check selection before legacy GB dispatch."""
import subprocess
import sys
import tempfile
from pathlib import Path

cli = sys.argv[1]


def run(*args, ok=True, contains=""):
    result = subprocess.run([cli, *map(str, args)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    assert contains in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result.stdout


listing = run("--list-systems", contains="sg1000 cpu=z80 inspect=yes disasm=yes codegen=desktop,meta,pokitto")
assert "gameboy cpu=sm83 inspect=yes disasm=yes codegen=desktop,meta,pokitto" in listing
run("--system", ok=False, contains="requires a value")
run("--cpu", "68000", ok=False, contains="unknown --cpu")
run("--system", "nes", ok=False, contains="unknown --system")
assert "sms cpu=z80" in listing and "gg cpu=z80" in listing
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    sg = root / "synthetic.SG"
    sg.write_bytes(bytes([0xc3, 0x00, 0x00]) + bytes(16381))
    info = run(sg, "--info", contains="system=sg1000\ncpu=z80")
    assert "rom_bytes=16384" in info and "work_ram_bytes=1024" in info
    run(sg, "--cpu", "sm83", "--info", ok=False, contains="requires CPU z80")
    run(sg, "--cpu", "z80", "--info", contains="mapper=unbanked")
    run(sg, "--info", "--disasm", ok=False, contains="cannot be combined")
    for target in ("desktop", "meta", "pokitto"):
        project = root / ("project-" + target)
        run(sg, "--target", target, "-o", project, contains="Generated SG-1000 project")
        assert (project / "src" / "sg_gen.c").is_file()
    run(sg, "--analyze", ok=False, contains="unsupported SG-1000 option")
    run(sg, "--disasm", "--limit", "1", contains="0000  JP $0000")
    run(sg, "--disasm", "--bank", "1", ok=False, contains="only have bank 0")
    raw = root / "synthetic.bin"
    raw.write_bytes(sg.read_bytes())
    run(raw, "--system", "sg1000", "--info", contains="system=sg1000")
    run(root / "missing.sg", "--info", ok=False, contains="Cannot read SG-1000")
    raw.write_bytes(bytes(0xc001))
    run(raw, "--system", "sg1000", "--info", ok=False, contains="mapper profile")
    raw.write_bytes(b"")
    run(raw, "--system", "sg1000", "--info", ok=False, contains="1 to 49152")
    raw.write_bytes(bytes([0x21, 0x00]))
    run(raw, "--system", "sg1000", "--disasm", ok=False, contains="truncated Z80")
    # Legacy parser permits homebrew headers. Minimum-sized ROM and size code 0.
    gb = root / "synthetic.gb"
    data = bytearray(32768)
    data[0x134:0x13c] = b"SYNTHETC"
    gb.write_bytes(data)
    run(gb, "--info", contains="system=gameboy\ncpu=sm83")
    run(gb, "--cpu", "z80", "--target", "meta", ok=False, contains="requires CPU sm83")
    run(gb, "--system", "gameboy", "--cpu", "sm83", "--info", contains="title=SYNTHETC")
    assert run(gb, "--disasm") == run(gb, "--system", "gameboy", "--disasm")
