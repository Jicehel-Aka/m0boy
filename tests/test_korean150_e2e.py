"""Synthetic 150-in-1 cartridge: six 8 KiB slots, generated native code,
flash streaming, indirect mapper writes, and rejection of old profiles.
No bytes are taken from a commercial cartridge."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import zlib


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=180)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout


def fixture():
    rom = bytearray([0xff]) * 0x200000
    for page in range(256):
        rom[page * 8192] = page
        rom[page * 8192 + 1] = 0xc9
        # The same callable routine in every physical page: count calls.
        rom[page * 8192 + 0x100:page * 8192 + 0x106] = bytes.fromhex('21 00 db 34 c9 00')
        rom[page * 8192 + 0x1ffe] = page
        rom[page * 8192 + 0x1fff] = 0x3e  # LD A,n across an 8 KiB boundary
    payload = bytearray()
    for mode in range(0, 256, 32):
        for bank in (0, 1, 2, 3, 0x3e, 0x3f):
            if bank & 1:
                payload += bytes((0x21, 0xfc, 0xff, 0x36, mode, 0x21, 0xf3, 0xff, 0x36, bank))
            else:
                payload += bytes((0x3e, mode, 0x32, 0xfc, 0xff, 0x3e, bank, 0x32, 0xf3, 0xff))
            for slot in range(6):
                address = slot * 8192 + 0x100
                payload += bytes((0xcd, address & 255, address >> 8))
                address = slot * 8192 + 0x1ffe
                payload += bytes((0x3a, address & 255, address >> 8, 0x32, slot + 1, 0xdb))
            payload += bytes((0xcd, 0xff, 0x1f))
    # Continue indefinitely in RAM; all calls above are compiled candidates.
    end = 0xc000 + len(payload)
    payload += bytes((0xc3, end & 255, end >> 8))
    assert len(payload) < 0x2000
    boot = bytes((0xf3, 0x31, 0xf0, 0xdf, 0x21, 0x00, 0x04,
                      0x11, 0x00, 0xc0, 0x01, len(payload) & 255, len(payload) >> 8,
                      0xed, 0xb0, 0x3e, 0x20, 0x32, 0xfc, 0xff,
                      0x3e, 0x02, 0x32, 0xf3, 0xff))
    rom[:len(boot)] = boot
    # This native store moves the very code being executed. Its successor
    # must come from page 4, not the already decoded page-0 instruction.
    rom[len(boot):len(boot)+8] = bytes.fromhex('3e ee 32 09 db c3 00 c0')
    at = 4 * 8192 + len(boot)
    rom[at:at+7] = bytes.fromhex('21 08 db 34 c3 00 c0')
    rom[0x400:0x400 + len(payload)] = payload
    # Identification is CRC-based. Solve the 32-bit linear CRC transform
    # for four padding bytes, rather than embedding a real cartridge.
    patch_at = len(rom) - 0x1000
    rom[patch_at:patch_at+4] = bytes(4)
    initial = zlib.crc32(rom)
    basis = {}
    for bit in range(32):
        rom[patch_at:patch_at+4] = (1 << bit).to_bytes(4, 'little')
        v, combination = zlib.crc32(rom) ^ initial, 1 << bit
        while v:
            pivot = v.bit_length() - 1
            if pivot not in basis:
                basis[pivot] = v, combination
                break
            other, mask = basis[pivot]
            v ^= other
            combination ^= mask
    delta, patch = 0x1b8956d1 ^ initial, 0
    while delta:
        value, mask = basis[delta.bit_length() - 1]
        delta ^= value
        patch ^= mask
    rom[patch_at:patch_at+4] = patch.to_bytes(4, 'little')
    assert zlib.crc32(rom) == 0x1b8956d1
    return rom


def fingerprint(s):
    return re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+) page=([0-9,]+).*run_hash=([0-9a-f]+)', s).groups()


p = argparse.ArgumentParser()
p.add_argument('--recompiler', required=True)
p.add_argument('--host', required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='korean150_') as temporary:
    root = Path(temporary)
    rom = root / 'synthetic.sms'
    rom.write_bytes(fixture())
    assert 'mapper=korean150' in run(a.recompiler, rom, '--info')
    copier = root / 'copier.sms'
    copier.write_bytes(bytes(512) + rom.read_bytes())
    assert 'mapper=korean150' in run(a.recompiler, copier, '--info')
    cov = root / 'profile.cov'
    reference = run(a.host, rom, '--frames', 10, '--coverage', cov)
    assert '# bank-shift 13' in cov.read_text()
    profile_tool = Path(__file__).resolve().parents[1] / 'tools/profile/m0profile.py'
    spec = importlib.util.spec_from_file_location('m0profile', profile_tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    probe = root / 'probe.cov'
    probe.write_text('# m0boy.sms.coverage.v2\n# bank-shift 13\n# frames 1\n800100 3\n')
    assert module.parse_cov(probe, 'sms')[0] == {0x100100: 3}
    assert re.search(r'^ff[a-f0-9]{4} ', cov.read_text(), re.M), 'last 8 KiB page covered'
    old = root / 'old.cov'
    old.write_text(cov.read_text().replace('# bank-shift 13\n', ''))
    bad = subprocess.run([a.recompiler, str(rom), '--target', 'desktop', '--coverage', str(old),
                          '-o', str(root / 'old')], capture_output=True, text=True)
    assert bad.returncode and '8 KiB mapper' in bad.stderr + bad.stdout
    for target in ('desktop', 'espboy'):
        project = root / target
        run(a.recompiler, rom, '--target', target, '--coverage', cov, '--meta-precise',
            '--no-scan', '--no-infer-data', '--output-prefix', 'synthetic', '-o', project)
        config = (project / 'src/sg_config.h').read_text()
        assert '#define GG_MAPPER 2' in config and '#define SG_PRECISE 1' in config
        if target == 'espboy':
            assert (project / 'rom.bin').stat().st_size == rom.stat().st_size - 16384
        build = project / 'host/build'
        run('cmake', '-G', 'Ninja', '-S', project / 'host', '-B', build)
        run('ninja', '-C', build)
        exe = build / 'synthetic_host'
        native = run(exe, '--frames', 10)
        interpreted = run(exe, '--frames', 10, '--interp')
        assert fingerprint(native) == fingerprint(interpreted) == fingerprint(reference), (native, interpreted, reference)
        assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
        print(target + ': six-window native/streamed mapper matches interpreter')
