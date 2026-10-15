#!/usr/bin/env python3
"""Optional local-ROM regression, not a compatibility certification.

Records a fresh short profile, generates a precise META storage plan and runs
its native and interpreted host paths against the independent generic host.
All projects and ROM-derived files must go in an ignored output directory.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import re
import subprocess
import shutil


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--recompiler', required=True, type=Path)
    p.add_argument('--host', required=True, type=Path)
    p.add_argument('--rom-root', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--frames', type=int, default=120)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--limit', type=int, default=0)
    p.add_argument('--resume', action='store_true', help='reuse successful checks already recorded in this output directory')
    p.add_argument('--clean-build', action='store_true', help='ninja-clean generated host binaries/objects after each successful check')
    args = p.parse_args()
    args.recompiler = args.recompiler.resolve()
    args.host = args.host.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    roms = sorted([*args.rom_root.glob('gg/*.gg'), *args.rom_root.glob('sms/*.sms')])
    if args.limit:
        roms = roms[:args.limit]
    previous = {}
    if args.resume and (out / 'results.jsonl').exists():
        for line in (out / 'results.jsonl').read_text().splitlines():
            try:
                result = json.loads(line)
                if result['ok']:
                    previous[result['rom']] = result
            except (ValueError, KeyError):
                pass  # a restart may leave one incomplete final line

    def exercise(item):
        index, rom = item
        if str(rom) in previous:
            return previous[str(rom)]
        folder = out / ('%04d' % index)
        log = []
        stage = 'prepare'

        def run(*cmd):
            result = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, timeout=180)
            log.append('$ ' + ' '.join(map(str, cmd)) + '\n' + result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError('command failed: ' + str(cmd[0]))
            return result.stdout

        def fingerprint(text):
            return re.search(r'run_hash=([0-9a-f]+)', text)[1]

        try:
            if shutil.disk_usage(out).free < 1024 * 1024 * 1024:
                raise RuntimeError('less than 1 GiB free; stop generating projects')
            folder.mkdir(exist_ok=True)
            common = ('--frames', args.frames, '--random', 201)
            profile = folder / 'coverage.cov'
            stage = 'reference'
            reference = fingerprint(run(args.host, rom.resolve(), *common, '--coverage', profile))
            project = folder / 'project'
            stage = 'generation'
            run(args.recompiler, rom.resolve(), '--target', 'meta', '--coverage', profile,
                '--meta-precise', '--meta-code-budget', 4096, '--output-prefix', 'storage', '-o', project)
            build = project / 'host/build'
            stage = 'host-build'
            run('cmake', '-G', 'Ninja', '-S', project / 'host', '-B', build)
            run('ninja', '-j1', '-C', build)
            exe = build / 'storage_host'
            stage = 'compare'
            native = fingerprint(run(exe, *common))
            interp = fingerprint(run(exe, *common, '--interp'))
            config = (project / 'src/sg_config.h').read_text()
            mode = 'packed' if '#define GG_ROM_PACKED 1' in config else \
                   'mixed' if '#define SG_ROM_STREAM 1' in config else 'raw'
            if native != reference or interp != reference:
                raise RuntimeError('hash mismatch: ref=%s native=%s interp=%s' % (reference, native, interp))
            if args.clean_build:
                run('ninja', '-C', build, '-t', 'clean')
            return dict(rom=str(rom), ok=True, mode=mode, run_hash=reference)
        except Exception as e:
            return dict(rom=str(rom), ok=False, stage=stage, error=str(e))
        finally:
            try:
                (folder / 'check.log').write_text('\n'.join(log))
            except OSError as e:
                print('Could not save log:', folder, e, flush=True)

    failures, modes = [], {}
    with (out / 'results.jsonl').open('w') as results, ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = [pool.submit(exercise, item) for item in enumerate(roms)]
        for number, future in enumerate(as_completed(jobs), 1):
            result = future.result()
            results.write(json.dumps(result) + '\n')
            results.flush()
            if not result['ok']:
                failures.append(result)
                print('FAIL', result, flush=True)
            else:
                modes[result['mode']] = modes.get(result['mode'], 0) + 1
            if number % 50 == 0:
                print('%d/%d checked, %d failures, modes=%s' % (number, len(roms), len(failures), modes), flush=True)
    print('%d ROMs, %d frames, seed 201: %d failures; %s' % (len(roms), args.frames, len(failures), modes))
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
