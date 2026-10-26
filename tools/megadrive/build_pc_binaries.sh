#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
rom=${1:-"$repo/roms/md/Sonic The Hedgehog (USA, Europe).md"}
out=${2:-"$repo/output/sonic1-pc"}
out=$(realpath -m "$out")
recompiler=${M0RECOMP:-"$repo/build/bin/m0recomp"}
rom=$(realpath "$rom")
mkdir -p "$out"

input="$out/sonic1.input"
coverage="$out/sonic1.cov"
printf '600 128\n602 0\n1050 8\n1150 0\n1300 64\n1310 0\n' > "$input"

build_native_pc() {
  local name=$1
  shift
  local project="$out/$name/project"
  if [[ $name == linux ]]; then
    "$recompiler" --system megadrive "$rom" --target pc -o "$project"
    cmake -G Ninja -S "$project" -B "$project/build" "$@"
    ninja -C "$project/build"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      "$project/build/md_pc" --interp --frames 1500 --input "$input" --coverage "$coverage"
  fi
  "$recompiler" --system megadrive "$rom" --target pc --coverage "$coverage" -o "$project"
  cmake -G Ninja -S "$project" -B "$project/build" "$@"
  ninja -C "$project/build"
}

build_native_pc linux
cp "$out/linux/project/build/md_pc" "$out/sonic1-linux"

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
build_native_pc windows -DCMAKE_TOOLCHAIN_FILE="$toolchain"
cp "$out/windows/project/build/md_pc.exe" "$out/sonic1.exe"
cp /usr/x86_64-w64-mingw32/bin/SDL2.dll "$out/SDL2.dll"
printf 'Built:\n  %s\n  %s (requires SDL2.dll beside it)\n' \
  "$out/sonic1-linux" "$out/sonic1.exe"
