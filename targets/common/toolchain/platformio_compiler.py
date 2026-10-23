"""Select the generated project's C/C++ compiler after PlatformIO setup."""
import os
from pathlib import Path
import shlex
import subprocess
import sys

Import('env', 'projenv')

compiler = os.environ.get('FIRMWARE_COMPILER', env.GetProjectOption('custom_compiler', 'gcc'))
if compiler not in ('gcc', 'clang'):
    raise ValueError('FIRMWARE_COMPILER/custom_compiler must be gcc or clang')
if compiler == 'clang':
    gcc = env.WhereIs(env.subst('$CC'))
    if not gcc:
        raise ValueError('PlatformIO GNU ARM compiler not found')
    wrapper = str(Path(env.subst('$PROJECT_DIR')) / 'toolchain' / 'arm_clang.py')
    python = env.subst('$PYTHONEXE')
    def command(language):
        args = [python, wrapper, '--gcc', gcc, '--language', language, '--']
        return subprocess.list2cmdline(args) if os.name == 'nt' else shlex.join(args)
    # Keep the already configured GNU link driver and assembler.
    linker = env.subst('$LINK')
    sys.path.insert(0, str(Path(wrapper).parent))
    from cmsis_compat import prepare_cmsis
    sdk = Path(env.PioPlatform().get_package_dir('framework-cmsis-atmel')) / 'CMSIS/Device/ATMEL'
    headers = prepare_cmsis(sdk, Path(env.subst('$BUILD_DIR')) / 'clang-cmsis')
    env.Depends(env.get('PIOBUILDFILES', []),
                [wrapper, str(Path(wrapper).with_name('cmsis_compat.py')),
                 str(Path(wrapper).with_name('platformio_compiler.py'))])
    for build_env in (env, projenv):
        build_env.Prepend(CPPPATH=[headers])
        build_env.Replace(CC=command('c'), CXX=command('c++'), LINK=linker)
