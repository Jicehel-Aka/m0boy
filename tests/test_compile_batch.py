"""Exercise batch routing/caching with synthetic ROMs and fake build tools."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='m0_batch_') as temporary:
    root = Path(temporary)
    shutil.copy2(SOURCE / 'compile.sh', root / 'compile.sh')
    (root / 'build/bin').mkdir(parents=True)
    commands = root / 'commands'
    commands.mkdir()
    tool = '''#!/usr/bin/env python3
import json,sys,pathlib,os
name=pathlib.Path(sys.argv[0]).name;args=sys.argv[1:]
with open('calls.jsonl','a') as f:f.write(json.dumps([name,args])+'\\n')
def value(flag,default=None):return args[args.index(flag)+1] if flag in args else default
if name=='gbrecomp':
 if args and args[0].endswith('-version'):print('0.8.1');sys.exit()
 out=pathlib.Path(value('-o'));(out/'src').mkdir(parents=True,exist_ok=True);(out/'host').mkdir(exist_ok=True)
 (out/'src/gbl_config.h').write_text('')
 (out/'mock_target').write_text(value('--target'))
 print('compiled: 1')
elif name=='sg1000_host':
 p=pathlib.Path(value('--coverage'));p.write_text('# m0boy.sg1000.coverage.v1\\n0000\\n')
elif name in ('pio','ninja','make'):
 out=pathlib.Path(value('-d') if name=='pio' else value('-C'))
 if name=='ninja' and out.name=='build':out=out.parent
 if (out/'mock_target').exists():
  target=(out/'mock_target').read_text();sg='sg1000' in out.parts
  build=out/('.pio/build/meta' if target=='meta' else 'build');build.mkdir(parents=True,exist_ok=True)
  (build/'firmware.bin').write_bytes((('m0boy-sg1000-' if sg else 'gbrecomp-')+target+' 0.8.1').encode())
 print('flash: 42; RAM: 32; Flash: 42')
elif name=='nproc':print('4')
'''
    for name in ('gbrecomp', 'sg1000_host', 'cmake', 'ninja', 'make', 'pio', 'nproc'):
        path = root / ('build/bin' if name in ('gbrecomp', 'sg1000_host') else 'commands') / name
        path.write_text(tool)
        path.chmod(0o755)
    for directory in ('gb', 'sg'):
        (root / 'roms' / directory).mkdir(parents=True)
    # Identical bytes under two source systems are separate cartridges.
    (root / 'roms/gb/Same.gb').write_bytes(b'synthetic')
    (root / 'roms/sg/Same.SG').write_bytes(b'synthetic')
    (root / 'roms/sg/Duplicate.sg').write_bytes(b'synthetic')
    (root / 'roms/sg/Second game.sg').write_bytes(b'other synthetic')
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'], TARGET='both',
               ALL='1', AUTO='0', STATIC='0', PACK='0', NO_ICONS='1', JOBS='1', PLAY='0')
    def run(*args, **overrides):
        result = subprocess.run(['bash', './compile.sh', *args], cwd=root,
                                env=dict(env, **overrides), text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout
    run()
    calls = [json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    generations = [args for name,args in calls if name=='gbrecomp' and '-o' in args]
    sg = [args for args in generations if '--system' in args]
    assert len(sg)==4, sg  # two SG cartridges, both destinations
    assert all(args[args.index('--system')+1]=='sg1000' for args in sg)
    assert all(not any(flag in args for flag in ('--meta-pack','--meta-static','--meta-bytes-per-instruction')) for args in sg)
    assert all(args[args.index('--meta-compiler')+1]=='gcc' for args in sg)  # recompiled Z80: firmware compiler forwarded
    assert any(name=='make' for name,args in calls)  # GB pipeline unchanged
    assert any(name=='cmake' and '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake' in args for name,args in calls)
    states = list((root/'roms').glob('*/**/.gbrmeta.json'))
    assert len(states)==6, states
    (root/'calls.jsonl').write_text('')
    output=run(PACK='1')  # SG ignores GB packing, already-built SG stays cached
    calls=[json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    assert not any(name=='gbrecomp' and '--system' in args for name,args in calls), calls
    assert 'Déjà à jour : 2' in output
    # Explicit SG directory; AUTO profile uses the SG host, not GB profiling tools.
    (root/'calls.jsonl').write_text('')
    run('roms/sg', 'output/sg card', TARGET='pokitto', AUTO='1', AUTO_SECONDS='2', FORCE='1', SG_SEEDS='2')
    calls=[json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
    profiles=[args for name,args in calls if name=='sg1000_host']
    # per cartridge: button 1 held, then SG_SEEDS random games
    assert len(profiles)==6 and all(args[args.index('--frames')+1]=='120' for args in profiles)
    assert sum('--random' in args for args in profiles)==4
    assert len(list((root/'roms/sg/profiles').glob('*/sg1000/coverage-auto.cov')))==2
    assert len(list((root/'roms/sg/profiles').glob('*/sg1000/coverage-rand*.cov')))==4
    assert len(list((root/'output/sg card').glob('*/.gbrmeta.json')))==2
    assert all('--coverage' in args for name,args in calls if name=='gbrecomp' and '--system' in args)
print('batch: mixed source systems, both targets, duplicate filtering, caching and SG profiles pass')
