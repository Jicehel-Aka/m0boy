#!/usr/bin/env bash
# Builds the tester archive of the META edition from a git revision.
#
#   targets/meta/package/make_package.sh <profiles_dir> [output.zip] [revision]
#
# <profiles_dir> holds recorded sessions keyed by ROM SHA-1
# (<sha1>/coverage-*.cov|.input, JEU.txt): addresses and inputs only.  The
# archive never contains ROMs or built firmwares.  Validate the result by
# extracting it elsewhere, building it with BUILD_TESTS=ON, running ctest
# and compile.sh on a ROM.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$here" rev-parse --show-toplevel)
profiles=${1:?usage: make_package.sh <profiles_dir> [output.zip] [revision]}
out=${2:-$root/output/gbrecomp-meta.zip}
rev=${3:-HEAD}
profiles=$(cd "$profiles" && pwd)
case "$out" in /*) ;; *) out=$PWD/$out ;; esac

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
pkg=$stage/gbrecomp-meta
mkdir -p "$pkg"
git -C "$root" archive "$rev" recompiler/CMakeLists.txt recompiler/include recompiler/src \
    targets/meta tests/test_host_configuration.c tests/test_meta_fat.py \
    tests/test_meta_target_e2e.py compile.sh LICENSE \
    tools/meta-compiler tools/gb-emu | tar -x -C "$pkg"
rm -rf "$pkg/targets/meta/package"
git -C "$root" show "$rev:targets/meta/package/CMakeLists.txt" > "$pkg/CMakeLists.txt"
git -C "$root" show "$rev:targets/meta/package/META.md" > "$pkg/META.md"
mkdir -p "$pkg/roms/gb"
git -C "$root" show "$rev:targets/meta/package/LISEZMOI.txt" > "$pkg/roms/gb/LISEZMOI.txt"
cp -r "$profiles" "$pkg/profiles"

# No ROM, firmware or installed Node packages may slip in.
if find "$pkg" -type d -name node_modules | grep -q .; then
    echo "refusing: node_modules in the package" >&2
    exit 1
fi
if find "$pkg" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.bin' -o -iname '*.elf' \
        -o -iname '*.sav' \) | grep -q .; then
    echo "refusing: ROM or binary in the package" >&2
    find "$pkg" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.bin' -o -iname '*.elf' -o -iname '*.sav' \) >&2
    exit 1
fi

rm -f "$out"
# Python's zipfile keeps the Unix modes (compile.sh stays executable).
(cd "$stage" && python3 -m zipfile -c "$out" gbrecomp-meta)
echo "$out ($(git -C "$root" rev-parse --short "$rev"), $(unzip -Z1 "$out" | grep -vc '/$') files)"
