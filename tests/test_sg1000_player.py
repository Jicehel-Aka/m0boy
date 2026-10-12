"""SDL player composes the reference core without losing frame/input timing."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
from test_sg1000_e2e import fixture

with tempfile.TemporaryDirectory(prefix='m0_sg_player_') as temporary:
    root = Path(temporary)
    rom = root / 'synthetic.sg'
    rom.write_bytes(fixture())
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    for executable, name in ((sys.argv[1], 'player'), (sys.argv[2], 'reference')):
        result = subprocess.run([executable, str(rom), '--frames', '24', '--start-frame', '8',
                        '--shot', str(root / (name + '.ppm'))], env=env, check=True,
                       capture_output=True, text=True, timeout=30)
        if name == "player":
            speed = re.search(r"speed_current=([0-9.]+)% speed_possible=([0-9.]+)%", result.stdout)
            assert speed and float(speed[2]) >= float(speed[1]) > 0, result.stdout
    assert (root / 'player.ppm').read_bytes() == (root / 'reference.ppm').read_bytes()
    paced = subprocess.run([sys.argv[1], str(rom), '--frames', '60', '--realtime'],
                           env=env, check=True, text=True, capture_output=True, timeout=30)
    speed = re.search(r"speed_current=([0-9.]+)% speed_possible=([0-9.]+)%", paced.stdout)
    assert speed and 0 < float(speed[1]) <= 101 and float(speed[2]) > float(speed[1]), paced.stdout
print('SDL player: input and pixels match the reference harness')
