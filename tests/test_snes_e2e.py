#!/usr/bin/env python3
"""Generate a synthetic LoROM and compare precise and fast code with the interpreter."""
import pathlib
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.STDOUT)


# The program's tail exercises what the region code only runs in the fast
# mode: the long forms and an open-bus read inside a region (the precise
# mode leaves before both).  A wrong long read or a stale open-bus value
# repaints the backdrop blue.
tail = bytes([
    0xc2, 0x20,                    # REP #$20: 16-bit accumulator
    0xa2, 0x00, 0x00,              # LDX #$0000
    0xbf, 0x00, 0x80, 0x00,        # LDA $008000,X: $105C (the bank's first bytes)
    0x8f, 0x00, 0x20, 0x7e,        # STA $7E2000: the long store into WRAM
    0xaf, 0x00, 0x20, 0x7e,        # LDA $7E2000: read back
    0xc9, 0x5c, 0x10,              # CMP #$105C
    0xe2, 0x20,                    # SEP #$20: 8-bit accumulator
    0xd0, 0x0d,                    # BNE fail (at $807F)
    0xad, 0x01, 0x42,              # LDA $4201: open bus returns the last fetch, $42
    0xc9, 0x42,                    # CMP #$42
    0xf0, 0x15,                    # BEQ ok (the idle loop at $808E)
    0x4c, 0x7f, 0x80,              # JMP fail
    0xea, 0xea, 0xea,              # pad up to $807F
    0xa9, 0x00, 0x8d, 0x21, 0x21,  # fail: CGADD = 0
    0xa9, 0x00, 0x8d, 0x22, 0x21,  # CGDATA low
    0xa9, 0x7c, 0x8d, 0x22, 0x21,  # CGDATA high: blue
    0x80, 0xfe,                    # ok: the idle loop
])

with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    rom = bytearray([0xea] * 0x8000)
    # Jump to the mirrored bank, set the backdrop red, update WRAM, then run
    # the tail.
    rom[0:4] = bytes([0x5c, 0x10, 0x80, 0x80])
    program = bytes([
        0x18, 0xfb, 0xc2, 0x20,       # native mode, 16-bit accumulator
        0xa9, 0x04, 0x01, 0x8d, 0x20, 0x01,  # store $0104
        0x4e, 0x20, 0x01,             # shift memory
        0xc2, 0x10, 0xa9, 0x00, 0x00, # 16-bit indexes, move one byte
        0xa2, 0x00, 0x80, 0xa0, 0x26, 0x01, 0x54, 0x7e, 0x00,
        0x4b, 0xab, 0xe2, 0x20,       # DBR = mirrored I/O bank; A is 8-bit
        0xa9, 0x00, 0x8d, 0x21, 0x21,  # CGADD = 0
        0xa9, 0x1f, 0x8d, 0x22, 0x21,  # red, low byte
        0xa9, 0x00, 0x8d, 0x22, 0x21,  # high byte
        0xa9, 0x0f, 0x8d, 0x00, 0x21,  # display enabled, full brightness
        0xa9, 0x53, 0x8d, 0x23, 0x01,  # WRAM mirror
        0xa9, 0x01, 0x8d, 0x16, 0x40,  # strobe controller
        0xa9, 0x00, 0x8d, 0x16, 0x40,
        0xad, 0x16, 0x40, 0x29, 0x01, 0x8d, 0x24, 0x01,  # save B
    ])
    rom[0x10:0x10 + len(program)] = program
    rom[0x10 + len(program):0x10 + len(program) + len(tail)] = tail
    assert rom[0x7f] == tail[0x7f - 0x5a] == 0xa9  # the fail block sits at $807F
    rom[0x7fd5] = 0x20
    rom[0x7ffc:0x7ffe] = bytes([0x00, 0x80])
    image = root / 'fixture.sfc'
    image.write_bytes(rom)
    project = root / 'project'
    report = run(sys.argv[1], '--system', 'snes', image, '--target', 'desktop',
                 '--output-prefix', 'synth', '-o', project, '--meta-precise')
    if 'instructions:' not in report:
        raise AssertionError(report)
    code = (project / 'src' / 'snes_gen.c').read_text()
    if '808010' not in code.lower():
        raise AssertionError('mirrored bank was not recompiled')
    run('cmake', '-G', 'Ninja', '-S', project / 'host', '-B', root / 'build')
    run('cmake', '--build', root / 'build')
    host = root / 'build' / 'synth_host'
    capture = root / 'screen.ppm'
    native = run(host, '--frames', '2', '--capture', capture)
    interp = run(host, '--frames', '2', '--interp')
    if native != interp or 'k=80' not in native:
        raise AssertionError(f'native: {native}interpreter: {interp}')
    header = b'P6\n256 224\n255\n'
    ppm = capture.read_bytes()
    if ppm != header + bytes([255, 0, 0]) * (256 * 224):
        raise AssertionError('unexpected backdrop capture')
    script = root / 'input.txt'
    script.write_text('0 b\n')
    scripted = run(host, '--frames', '2', '--input', script)
    scripted_interp = run(host, '--frames', '2', '--input', script, '--interp')
    if scripted != scripted_interp or scripted == native:
        raise AssertionError(f'input native: {scripted}interpreter: {scripted_interp}')

    # The fast mode compiles what the precise mode leaves: the long forms and
    # the open-bus read must stay identical to the interpreter there too.
    fast_project = root / 'fast-project'
    run(sys.argv[1], '--system', 'snes', image, '--target', 'desktop',
        '--output-prefix', 'fast', '-o', fast_project)
    fast_code = (fast_project / 'src' / 'snes_gen.c').read_text()
    if 'rd24' not in fast_code.lower():
        raise AssertionError('the long forms were not recompiled in the fast mode')
    run('cmake', '-G', 'Ninja', '-S', fast_project / 'host', '-B', root / 'fast-build')
    run('cmake', '--build', root / 'fast-build')
    fast_host = root / 'fast-build' / 'fast_host'
    fast_native = run(fast_host, '--frames', '2', '--capture', root / 'fast.ppm')
    fast_interp = run(fast_host, '--frames', '2', '--interp')
    if fast_native != fast_interp:
        raise AssertionError(f'fast native: {fast_native}interpreter: {fast_interp}')
    if (root / 'fast.ppm').read_bytes() != header + bytes([255, 0, 0]) * (256 * 224):
        raise AssertionError('unexpected fast-mode backdrop capture')

    # The generated STP must leave the same bus and sleep state as the interpreter.
    rom[0x10 + len(program) + len(tail) - 2] = 0xdb
    image_path = root / 'stop.sfc'
    image_path.write_bytes(rom)
    stop_project = root / 'stop-project'
    run(sys.argv[1], '--system', 'snes', image_path, '--target', 'desktop',
        '--output-prefix', 'stop', '-o', stop_project, '--meta-precise')
    run('cmake', '-G', 'Ninja', '-S', stop_project / 'host', '-B', root / 'stop-build')
    run('cmake', '--build', root / 'stop-build')
    stop_host = root / 'stop-build' / 'stop_host'
    native = run(stop_host, '--frames', '2')
    interp = run(stop_host, '--frames', '2', '--interp')
    if native != interp:
        raise AssertionError(f'STP native: {native}interpreter: {interp}')
