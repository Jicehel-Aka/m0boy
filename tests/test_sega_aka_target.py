#!/usr/bin/env python3
"""AKA Sega projects: complete drivers, host parity, optional real firmware build
and native Xtensa run via AKA_PIO and AKA_GM0. Synthetic cartridges only."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
from gg_fixture import fixture as gg_fixture
from test_sg1000_e2e import fixture as sg_fixture
from test_meta_target_e2e import run, build_host

def parse(text):
    match=re.search(r"run_hash=([0-9a-f]+)",text)
    assert match,text
    if "out_mismatch=" in text:
        assert "out_mismatch=0" in text,text
    return {"run_hash": match.group(1)}

def main():
    cli=sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='sega-aka-') as tmp:
        tmp=Path(tmp)
        for system, data in [('sg1000',sg_fixture()),('gg',gg_fixture()),('sms',gg_fixture())]:
            rom=tmp/('test.'+system);rom.write_bytes(data)
            project=tmp/system
            run([cli,str(rom),'--system',system,'--target','aka','--meta-static','--meta-precise',
                 '--output-prefix','e2e','-o',str(project)])
            assert (project/'boards/gamebuino_aka.json').is_file()
            if system!='sg1000':assert (project/'src/gg_stream.c').is_file()
            build_host(project)
            results=[parse(run([str(project/'host/build/e2e_host'),'--frames','60',*mode]))
                     for mode in ([],['--interp'])]
            assert results[0]['run_hash']==results[1]['run_hash'],results
            pio=os.environ.get('AKA_PIO')
            if pio:
                result=subprocess.run([pio,'run','-d',str(project),'-j','2'],capture_output=True,text=True,timeout=600)
                assert result.returncode==0,result.stdout[-3000:]+result.stderr[-3000:]
                firmware=project/'.pio/build/aka/firmware.bin'
                assert firmware.stat().st_size<=0x2EE000
                gm0=os.environ.get('AKA_GM0')
                if gm0:
                    env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',EMU_NOPACE='1')
                    shot=project/'screen.ppm'
                    result=subprocess.run([gm0,str(firmware.with_suffix('.elf')),'--frames','12','--shot',str(shot)],
                                          capture_output=True,text=True,env=env,timeout=120)
                    assert result.returncode==0 and 'AKA:' not in result.stderr,result.stdout+result.stderr
                    pixels=shot.read_bytes().split(b'\n255\n')[1]
                    assert len(pixels)==320*240*3
                    assert len(set(pixels[i:i+3] for i in range(0,len(pixels),3)))>1,'blank '+system
            print(system+': AKA project and precise/interpreter parity checked')

if __name__=='__main__':main()
