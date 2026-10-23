"""ROM access traces (gg_host --rom-trace-out) and the simulated card cache
(m0romsim): what the storage plan of tools/profile stands on."""
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from gg_fixture import fixture

romsim, gg_host = sys.argv[1], sys.argv[2]


def token(v):
    out = bytearray()
    while True:
        b = v & 127
        v >>= 7
        out.append(b | 128 if v else b)
        if not v:
            return bytes(out)


def trace(path, rom_bytes, frames):
    """frames: lists of (offset, kind); the writer's encoding, consecutive
    duplicates kept out as the harness does."""
    body, last, last_kind = bytearray(), None, None
    for accesses in frames:
        for offset, kind in accesses:
            g = offset >> 5
            if g == last and kind == last_kind:
                continue
            d = g if last is None else g - last
            z = (-d << 1) - 1 if d < 0 else d << 1
            body += token((z << 1 | kind) + 1)
            last, last_kind = g, kind
        body += b'\0'
    with gzip.open(path, 'wb') as f:
        f.write(f'm0boy.romtrace.v1 {rom_bytes}\n'.encode() + bytes(body))


def sim(*args):
    r = subprocess.run([romsim, *map(str, args)], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


with tempfile.TemporaryDirectory(prefix='m0_romsim_') as temporary:
    root = Path(temporary)
    rom = root / 'rom.gg'
    data = bytearray(0x10000)
    for i in range(0x4000, 0x10000):
        data[i] = (i * 2654435761 >> 13) & 255            # noise: raw beats LZSS
    data[0x8000:0x8200] = bytes(512)                      # one sector LZSS shrinks to little
    rom.write_bytes(bytes(data))

    # Page 0 never reaches the card; one sector read again and again is one
    # SD read (staged, then cached); two blocks of one sector, one read.
    t = root / 'a.trace.gz'
    trace(t, len(data), [[(0x100, 0), (0x4000, 0), (0x4040, 0)], [(0x4000, 0)] * 3, [(0x10, 1)]])
    r = sim('--ways', 4, rom, t)
    assert r['page0_only']['frames'] == 3
    assert r['page0_only']['sd_reads'] == 1, r['page0_only']
    assert r['page0_only']['sd_reads_per_frame']['max'] == 1

    # A working set larger than the cache misses every time round: 6 sectors,
    # 4 ways (3 usable before 0.10.35's fix: the same here).
    frames = [[(0x4000 + 512 * k, 0) for k in range(6)] for _ in range(10)]
    trace(t, len(data), frames)
    base = sim('--ways', 4, rom, t)['page0_only']
    assert base['sd_reads'] == 60, base
    legacy = sim('--ways', 4, '--legacy-mru', rom, t)['page0_only']
    assert legacy['sd_reads'] == 60
    # A budget that holds them all in flash: no SD read left; the plan obeys
    # its budget and replays to the same figures.
    plan = root / 'plan'
    r = sim('--ways', 4, '--budget', 6 * 516, '--sizes', '512', '--plan', plan, rom, t)
    assert r['result']['sd_reads'] == 0, r['result']
    assert r['plan']['flash_bytes'] <= 6 * 516
    again = sim('--ways', 4, '--simulate-plan', plan, rom, t)['given_plan']['result']
    assert again['sd_reads'] == 0
    lines = [l.split() for l in plan.read_text().splitlines() if not l.startswith('#')]
    assert all(int(l[0], 16) % 512 == 0 and l[1] == '512' and float(l[4]) >= 0 for l in lines), lines
    # The all-zero sector is cheaper packed: with room for little else, LZSS.
    trace(t, len(data), [[(0x8000, 0)] for _ in range(50)] + [[(0x4000 + 512 * k, 0) for k in range(6)] for _ in range(2)])
    r = sim('--ways', 1, '--budget', 64, '--sizes', '512', '--plan', plan, rom, t)
    assert any(l.split()[2] == 'lzss' for l in plan.read_text().splitlines() if l.startswith('008000')), plan.read_text()

    # The harness writes what the simulator reads: one frame mark per frame.
    gg = root / 'fixture.gg'
    gg.write_bytes(fixture())
    out = root / 'fixture.trace.gz'
    run = subprocess.run([gg_host, str(gg), '--frames', '30', '--coverage', str(root / 'c.cov'),
                          '--rom-trace-out', str(out)], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    assert 'rom_trace:' in run.stderr and out.stat().st_size > 30, run.stderr
    r = sim(gg, out)
    assert r['page0_only']['frames'] == 30 and r['page0_only']['accesses'] > 0, r['page0_only']
    # --rom-trace-skip: compiled instructions leave the trace
    every = int(run.stderr.split('rom_trace: ')[1].split()[0])
    skip = root / 'compiled.txt'
    skip.write_text('# all of page 0\n' + ''.join(f'{a:x}\n' for a in range(0, 0x4000)))
    run = subprocess.run([gg_host, str(gg), '--frames', '30', '--coverage', str(root / 'c.cov'),
                          '--rom-trace-out', str(out), '--rom-trace-skip', str(skip)], capture_output=True, text=True, timeout=120)
    fewer = int(run.stderr.split('rom_trace: ')[1].split()[0])
    assert run.returncode == 0 and fewer < every, (fewer, every)
    # Version 2: 16-byte granules, repeats, cartridge RAM.
    def trace2(path, rom_bytes, frames, cart_bytes=32768):
        """frames: lists of ('r'|'f', offset, repeats) or ('c', offset, write)"""
        body, last, cart_last = bytearray(), 0, 0
        for events in frames:
            for e in events:
                if e[0] == 'again':  # repeats of the access before, after an end of frame
                    body += token((e[1] << 2 | 2) + 1)
                    continue
                if e[0] == 'c':
                    g = e[1] >> 4
                    d = g - cart_last
                    z = (-d << 1) - 1 if d < 0 else d << 1
                    body += token((((z << 1) | e[2]) << 2 | 3) + 1)
                    cart_last = g
                    continue
                g = e[1] >> 4
                d = g - last
                z = (-d << 1) - 1 if d < 0 else d << 1
                body += token((z << 2 | (e[0] == 'f')) + 1)
                last = g
                if e[2]:
                    body += token((e[2] << 2 | 2) + 1)
            body += b'\0'
        with gzip.open(path, 'wb') as f:
            f.write(f'm0boy.romtrace.v2 {rom_bytes} {cart_bytes}\n'.encode() + bytes(body))

    # each repeat is one more read: the first misses, the nine others find
    # the block read last
    trace2(t, len(data), [[('r', 0x4000, 9)]])
    r = sim('--ways', 4, '--runtime', 'lean', rom, t)['page0_only']
    assert r['accesses'] == 10 and r['classes']['cache_miss'] == 1 and r['classes']['cache_last'] == 9, r
    assert r['cycles_per_frame']['sd'] > 0 and r['cycles_per_frame']['access'] > 0, r
    # cartridge RAM in the card cache: a changed block is written back 120
    # frames after its last change
    trace2(t, len(data), [[('c', 0x100, 1)]] + [[] for _ in range(130)])
    r = sim('--ways', 4, '--cart-on-card', 1, rom, t)['page0_only']
    assert r['classes']['cart_miss'] == 1 and r['sd_writes'] == 1, r
    assert sim('--ways', 4, rom, t)['page0_only']['sd_writes'] == 0  # cartridge RAM in RAM
    # a write repeated in every frame is a change in every frame: never 120 quiet ones
    trace2(t, len(data), [[('c', 0x100, 1)]] + [[('again', 1)] for _ in range(130)])
    r = sim('--ways', 4, '--cart-on-card', 1, rom, t)['page0_only']
    assert r['sd_writes'] == 0, r
    # one way each for code and data: no thrashing where one shared way
    # would; a unified cache of one way reloads at every switch
    trace2(t, len(data), [[('f', 0x4000, 0), ('r', 0x8000, 0)] * 8])
    split = sim('--split', '1,1', rom, t)['page0_only']
    unified = sim('--ways', 1, rom, t)['page0_only']
    assert split['cache_misses'] == 2 and unified['cache_misses'] == 16, (split, unified)

    # the harness writes version 2: repeats and interpreted bytes counted
    run = subprocess.run([gg_host, str(gg), '--frames', '30', '--coverage', str(root / 'c.cov'),
                          '--rom-trace-out', str(out)], capture_output=True, text=True, timeout=120)
    with gzip.open(out, 'rb') as f:
        assert f.readline().startswith(b'm0boy.romtrace.v2 '), 'header'
    r = sim(gg, out)['page0_only']
    assert r['accesses'] == int(run.stderr.split('rom_trace: ')[1].split()[0]), (r['accesses'], run.stderr)
print('romsim: traces, card cache, plans and the harness agree')
