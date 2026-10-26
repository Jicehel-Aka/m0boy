#!/usr/bin/env bash
# Convertit une ou plusieurs ROMs en jeux pour la Gamebuino AKA : un .zip par jeu,
# a decompresser a la racine de la carte SD (le Launcher le liste ensuite).
#
#   tools/aka/convert.sh jeu.gg                  -> ./aka-out/jeu.zip
#   tools/aka/convert.sh jeu1.sms jeu2.gb dossier/   (fichiers et/ou dossiers de ROMs)
#   tools/aka/convert.sh -o ma_sortie -r jeu.gg   (-o : dossier de sortie, -r : rapide)
#
# Systemes pris en charge pour l'AKA : Game Boy/Color (.gb .gbc), SG-1000 (.sg),
# Game Gear (.gg), Master System (.sms). Le Mega Drive n'est pas pris en charge.
# Prerequis : CMake, Ninja, un compilateur C/C++, zlib (dev), Python 3, PlatformIO.
# Sous Windows : utiliser convert.bat (WSL) ou deposer la ROM sur convert.bat.
set -u
ici="$(cd "$(dirname "$0")" && pwd)"
racine="$(cd "$ici/../.." && pwd)"
sortie="$PWD/aka-out"
rapide=0
entrees=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) sortie="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2 ;;
    -r|--rapide) rapide=1; shift ;;
    -h|--help) sed -n 2,12p "$0"; exit 0 ;;
    *) entrees+=("$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")"); shift ;;
  esac
done
[ ${#entrees[@]} -gt 0 ] || { echo "Usage : $0 [-o dossier] [-r] rom_ou_dossier ..."; exit 1; }

manque=""
for c in cmake ninja python3 pio; do command -v "$c" >/dev/null 2>&1 || manque="$manque $c"; done
if [ -n "$manque" ]; then
  echo "Outils manquants :$manque"
  echo "  Debian/Ubuntu/WSL : sudo apt install cmake ninja-build g++ zlib1g-dev python3 pipx && pipx install platformio"
  exit 1
fi

# Recompilateur : construit une fois.
if [ ! -x "$racine/build/bin/m0recomp" ]; then
  echo "[aka] premiere utilisation : construction du recompilateur..."
  ( cd "$racine" && cmake -G Ninja -B build . >/dev/null && ninja -C build ) || { echo "Echec de la construction"; exit 1; }
fi

# Les ROMs sont copiees dans un dossier de travail propre (compile.sh lit un dossier).
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
n=0
for e in "${entrees[@]}"; do
  if [ -d "$e" ]; then
    while IFS= read -r -d '' f; do cp "$f" "$tmp/"; n=$((n+1)); done \
      < <(find "$e" -maxdepth 2 -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.sg' -o -iname '*.gg' -o -iname '*.sms' \) -print0)
  elif [ -f "$e" ]; then
    case "${e,,}" in
      *.gb|*.gbc|*.sg|*.gg|*.sms) cp "$e" "$tmp/"; n=$((n+1)) ;;
      *.md|*.bin|*.gen|*.smd) echo "[aka] $(basename "$e") : Mega Drive non pris en charge sur l'AKA, ignore." ;;
      *.zip) ( cd "$tmp" && unzip -o -q "$e" ) && n=$((n+1)) ;;
      *) echo "[aka] $(basename "$e") : format inconnu, ignore." ;;
    esac
  else
    echo "[aka] introuvable : $e"
  fi
done
[ "$n" -gt 0 ] || { echo "Aucune ROM utilisable."; exit 1; }

export TARGET=aka
if [ "$rapide" = 1 ]; then export AUTO_SECONDS=20 SG_SEEDS=1 EXPLORERS=0; fi
brut="$tmp/_sortie"
( cd "$racine" && ./compile.sh "$tmp" "$brut" < /dev/null ) || true

mkdir -p "$sortie"
nb=0
while IFS= read -r -d '' z; do cp "$z" "$sortie/"; nb=$((nb+1)); done < <(find "$brut" -name '*.zip' -print0 2>/dev/null)
if [ "$nb" -eq 0 ]; then echo; echo "Aucun jeu produit : voir les messages ci-dessus."; exit 1; fi
echo; echo "[aka] $nb jeu(x) dans : $sortie"
echo "      Decompresse chaque .zip a la racine de la carte SD, puis lance-le depuis le Launcher."
