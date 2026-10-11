"""Local compatibility view of the SAMD21 SDK for Clang C++.

Old Atmel headers qualify unnamed reserved bitfields, a GCC extension that
Clang rejects. Removing qualifiers from unnamed fields leaves register
layout and every named volatile/readonly field unchanged. SDK files are
never edited: only a build-directory copy is used in Clang mode.
"""
from pathlib import Path
import re

_RESERVED = re.compile(r'(?m)^([ \t]*)(?:__I|__O|__IO)([ \t]+u?int(?:8|16|32|64)_t[ \t]*:[ \t]*\d+[ \t]*;)')


def sanitize_reserved_fields(text):
    return _RESERVED.sub(r'\1\2', text)


def prepare_cmsis(source, destination):
    source, destination = Path(source), Path(destination)
    paths = [source / 'sam.h'] + sorted(p for p in source.glob('*.h') if p.name != 'sam.h')
    paths += sorted((source / 'samd21/include').rglob('*.h'))
    if not paths[0].is_file() or not (source / 'samd21/include').is_dir():
        raise ValueError('SAMD21 CMSIS headers not found in ' + str(source))
    for path in paths:
        data = sanitize_reserved_fields(path.read_bytes().decode('latin1')).encode('latin1')
        target = destination / path.relative_to(source)
        if target.is_file() and target.read_bytes() == data:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return str(destination)
