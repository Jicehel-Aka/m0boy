"""gm0 sessions (--record-input) become host --input files that replay as scripts do."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from test_sg1000_e2e import fixture

SOURCE = Path(__file__).resolve().parents[1]
TOOL = SOURCE / 'tools/profile/m0profile.py'
host = sys.argv[1]

with tempfile.TemporaryDirectory(prefix='m0_sessions_') as temporary:
    root = Path(temporary)
    rom = root / 'synthetic.sg'
    rom.write_bytes(fixture())
    store = root / 'store'
    store.mkdir()
    (store / 'rom.env').write_text(f"NAME='Synthetic'\nSYSTEM='sg1000'\nROM='{rom}'\n")

    def session(lines, sid):
        keys = store / f'{sid}.keys'
        keys.write_text('# gm0 input v1 target=meta\n' + ''.join(line + '\n' for line in lines))
        out = subprocess.run([sys.executable, str(TOOL), 'session', str(store), str(keys), sid],
                             check=True, capture_output=True, text=True).stdout
        return int(out), store / f'coverage-{sid}.input'

    # Game frames and button states as the firmware's m0input descriptor
    # exposes them (SG-1000: pad byte, pause in the extra byte).
    frames, path = session(['k 0 00', 'g 0 0 0000', 'k 40 10', 'g 40 31 0010',
                            'g 52 43 0009', 'g 60 51 0100', 'g 61 52 0000', 'e 90 81'], 'a')
    assert frames == 81
    assert path.read_text().splitlines()[1:] == ['0 none', '31 1', '43 up+right', '51 pause', '52 none', '81 none']
    # A state loaded (counter back) or a restart: what follows cannot replay from power-on.
    frames, path = session(['g 0 0 0000', 'g 10 9 0010', 'g 20 5 0000', 'g 30 15 0020', 'e 40 25'], 'b')
    assert frames == 9 and 'cut:' in path.read_text().splitlines()[0], path.read_text()
    frames, path = session(['g 0 0 0000', 'g 10 9 0001', 'r 15', 'g 16 1 0000', 'e 40 25'], 'c')
    assert frames == 9 and 'restart' in path.read_text()
    frames, _ = session(['k 0 00', 'e 40 0'], 'd')  # firmware without descriptor
    assert frames == 0 and not (store / 'coverage-d.input').exists()

    # The harness reads the file as it reads the equivalent script.
    def run(*extra, name):
        cov, shot = root / f'{name}.cov', root / f'{name}.ppm'
        subprocess.run([host, str(rom), '--frames', '90', '--coverage', str(cov), '--shot', str(shot), *extra],
                       check=True, capture_output=True, timeout=60)
        return cov.read_text(), shot.read_bytes()
    filed = run('--input', str(store / 'coverage-a.input'), name='file')
    scripted = run('--input', '31:16:12,43:9:8', '--pause-frame', '51', name='script')
    assert filed == scripted
    assert subprocess.run([host, str(rom), '--frames', '2', '--input', str(root / 'missing.input')],
                          capture_output=True, timeout=60).returncode != 0

    # profile.json counts the replayed session as a gm0 one.
    subprocess.run([host, str(rom), '--frames', '82', '--input', str(store / 'coverage-a.input'),
                    '--coverage', str(store / 'coverage-gm0-20261022-120000.cov')], check=True, capture_output=True)
    subprocess.run([sys.executable, str(TOOL), 'report', str(store), '--quiet'], check=True)
    report = json.loads((store / 'profile.json').read_text())
    assert [s['kind'] for s in report['sessions']] == ['gm0'], report['sessions']
print('profile sessions: gm0 recordings convert, cut and replay as host scripts')
