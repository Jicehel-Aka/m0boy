#!/usr/bin/env bash
# Builds the release archive of m0boy from a git revision.
#
#   targets/meta/package/make_package.sh [profiles_dir] [output.zip] [revision]
#
# The archive is the repository tree at <revision> (default HEAD), in a
# m0boy-<targets/meta/VERSION>/ folder; the default output is
# output/m0boy-<version>.zip.  [profiles_dir] optionally adds recorded
# sessions keyed by ROM SHA-1 (<sha1>/coverage-*.cov|.input, JEU.txt):
# addresses and inputs only.  The archive never contains ROMs or built
# firmwares.  Validate the result by extracting it elsewhere, building it
# with BUILD_TESTS=ON, running ctest and compile.sh on a ROM.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$here" rev-parse --show-toplevel)
profiles=${1:-}
rev=${3:-HEAD}
version=$(git -C "$root" show "$rev:targets/meta/VERSION" | head -n1)
out=${2:-$root/output/m0boy-$version.zip}
case "$out" in /*) ;; *) out=$PWD/$out ;; esac

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
name=m0boy-$version
pkg=$stage/$name
mkdir -p "$pkg"
git -C "$root" archive "$rev" | tar -x -C "$pkg"
if [ -n "$profiles" ]; then
    cp -r "$(cd "$profiles" && pwd)/." "$pkg/profiles/"
fi

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

mkdir -p "$(dirname "$out")"
rm -f "$out"
# Python's zipfile keeps the Unix modes (compile.sh stays executable).
(cd "$stage" && python3 -m zipfile -c "$out" "$name")
echo "$out (v$version, $(git -C "$root" rev-parse --short "$rev"), $(unzip -Z1 "$out" | grep -vc '/$') files)"
