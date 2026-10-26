#!/usr/bin/env bash
set -euo pipefail

# PC Engine PC binaries: the interpreter runtime with a HuCard embedded,
# an SDL2 window on Linux and a Windows .exe (MinGW cross-build, SDL2.dll
# beside it).  Model: tools/megadrive/build_pc_binaries.sh.

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
rom=${1:-"$repo/roms/pce/Raiden (USA).pce"}
out=${2:-"$repo/output/raiden-pc"}
out=$(realpath -m "$out")
recompiler=${M0RECOMP:-"$repo/build/bin/gbrecomp"}
rom=$(realpath "$rom")
mkdir -p "$out"

build_pc() {
  local name=$1
  shift
  local project="$out/$name/project"
  "$recompiler" --system pce "$rom" --target pc -o "$project"
  cmake -G Ninja -S "$project" -B "$project/build" "$@"
  ninja -C "$project/build"
}

# the project prefix, as main.cpp's sanitize_prefix
name=$(basename "$rom")
name=${name%.*}
name=$(printf '%s' "$name" | tr -c 'a-zA-Z0-9_' '_')

build_pc linux
cp "$out/linux/project/build/${name}_pc" "$out/raiden-linux"

toolchain="$out/mingw-toolchain.cmake"
cat > "$toolchain" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig
export PKG_CONFIG_PATH=
build_pc windows -DCMAKE_TOOLCHAIN_FILE="$toolchain"
cp "$out/windows/project/build/${name}_pc.exe" "$out/raiden.exe"
cp /usr/x86_64-w64-mingw32/bin/SDL2.dll "$out/SDL2.dll"
printf 'Built:\n  %s\n  %s (requires SDL2.dll beside it)\n' \
  "$out/raiden-linux" "$out/raiden.exe"
