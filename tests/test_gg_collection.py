"""Opt-in GG/SMS collection regression; ROMs and results stay outside Git.

Compare interpreter output to coverage-guided precise native code, optionally
also to a saved pre-change gg_host. Each cartridge uses an isolated project.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time


def fingerprint(output):
    match = re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+) page=([0-9,]+).*run_hash=([0-9a-f]+)', output)
    if not match:
        raise RuntimeError('Missing runtime fingerprint: ' + output[-1000:])
    return match.groups()


def run(command, log, timeout=120):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError('Command failed: ' + str(log) + '\n' + result.stdout[-1000:] + result.stderr[-1000:])
    return result.stdout


def cartridge(rom, args):
    identity = hashlib.sha256(str(rom).encode()).hexdigest()[:16]
    root = args.output / identity
    root.mkdir(parents=True, exist_ok=True)
    result = {'rom': str(rom), 'system': rom.suffix.lower()[1:], 'directory': str(root)}
    started = time.monotonic()
    try:
        common = ['--frames', args.frames, '--random', 42, '--start-frame', 30]
        reference = run([args.host, rom, *common, '--coverage', root / 'coverage.cov',
                         '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav'], root / 'reference.log')
        if args.baseline_host:
            old = run([args.baseline_host, rom, *common, '--shot', root / 'before.ppm',
                       '--wav', root / 'before.wav'], root / 'before.log')
            assert fingerprint(old) == fingerprint(reference), 'Before/after interpreter state differs'
            for extension in ('ppm', 'wav'):
                assert (root / ('before.' + extension)).read_bytes() == (root / ('reference.' + extension)).read_bytes(), 'Before/after ' + extension + ' differs'
        project = root / 'project'
        run([args.recompiler, rom, '--target', 'desktop', '--coverage', root / 'coverage.cov',
             '--meta-precise', '--meta-code-budget', args.code_budget,
             '--output-prefix', 'cartridge', '-o', project], root / 'generate.log')
        build = project / 'host/build'
        if args.runtime_libs:
            # Reuse the same generic runtime as gg_host; generated regions
            # still specialise their bus/mapper from each sg_config.h.
            # The generic initializer needs the system supplied explicitly.
            harness = project / 'host/gg_host.c'
            source = harness.read_text()
            assert 'gg_init(rom, size, cart);' in source
            harness.write_text(source.replace('gg_init(rom, size, cart);', 'gg_init_system(rom, size, cart, GG_SMS);'))
            cmake = project / 'host/CMakeLists.txt'
            cmake.write_text('cmake_minimum_required(VERSION 3.20)\nproject(collection C)\n'
                             'set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n'
                             'add_executable(cartridge_host gg_host.c ${SRC}/sg_gen.c ${SRC}/sg_rom.c)\n'
                             'target_include_directories(cartridge_host PRIVATE ${SRC})\n'
                             'target_compile_definitions(cartridge_host PRIVATE SG_GENERATED=1 GG_ROM_TRACE=1)\n'
                             f'target_link_libraries(cartridge_host PRIVATE "{args.runtime_libs}/libm0boy_gg_runtime.a" "{args.runtime_libs}/libm0boy_z80_runtime.a")\n')
        configure = ['cmake', '-G', 'Ninja', '-S', project / 'host', '-B', build,
                     '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG']
        if args.compiler:
            configure += ['-DCMAKE_C_COMPILER=' + args.compiler]
        run(configure, root / 'configure.log')
        run(['ninja', '-C', build, '-j', 1], root / 'build.log')
        native = run([build / 'cartridge_host', *common, '--shot', root / 'native.ppm',
                      '--wav', root / 'native.wav'], root / 'native.log')
        assert fingerprint(native) == fingerprint(reference), 'Precise native/interpreter state differs'
        for extension in ('ppm', 'wav'):
            assert (root / ('native.' + extension)).read_bytes() == (root / ('reference.' + extension)).read_bytes(), 'Native/interpreter ' + extension + ' differs'
        result.update(status='passed', fingerprint=fingerprint(native),
                      compiled=int(re.search(r'compiled=(\d+)', native)[1]),
                      interpreted=int(re.search(r'interpreted=(\d+)', native)[1]))
    except Exception as error:
        result.update(status='failed', error=str(error))
    result['seconds'] = round(time.monotonic() - started, 2)
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom-dir', type=Path, required=True)
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--baseline-host', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=360)
    parser.add_argument('--code-budget', type=int, default=65536)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--runtime-libs', type=Path, help='Reuse the generic gg_host runtime libraries for a large collection')
    parser.add_argument('--compiler', help='Host C compiler (e.g. clang)')
    parser.add_argument('--resume', action='store_true', help='Keep successful cases from an interrupted run with the same inputs and sources')
    args = parser.parse_args()
    for name in ('rom_dir', 'recompiler', 'host', 'baseline_host', 'output', 'runtime_libs'):
        value = getattr(args, name)
        if value:
            setattr(args, name, value.resolve())
    roms = sorted(p for p in args.rom_dir.rglob('*') if p.is_file() and p.suffix.lower() in ('.gg', '.sms'))
    if not roms:
        parser.error('No .gg/.sms cartridges found')
    if args.frames < 1 or args.jobs < 1 or args.code_budget < 1:
        parser.error('Frames, jobs and code budget must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.compiler:
        args.compiler = shutil.which(args.compiler) or args.compiler
    results = []
    pending = []
    for rom in roms:
        identity = hashlib.sha256(str(rom).encode()).hexdigest()[:16]
        saved = args.output / identity / 'result.json'
        previous = json.loads(saved.read_text()) if args.resume and saved.exists() else None
        if previous and previous['status'] == 'passed':
            results.append(previous)
        else:
            pending.append(rom)
    print(f'{len(results)} successful cases resumed; {len(pending)} to run', flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(cartridge, rom, args) for rom in pending]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            if result['status'] == 'failed' or len(results) % 10 == 0 or len(results) == len(roms):
                print(f"{len(results)}/{len(roms)} passed={sum(r['status'] == 'passed' for r in results)} latest={result['status']} {result['rom']}", flush=True)
            (args.output / 'results.json').write_text(json.dumps(sorted(results, key=lambda r: r['rom']), indent=2) + '\n')
    return int(any(result['status'] == 'failed' for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
