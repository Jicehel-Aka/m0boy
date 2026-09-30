#!/usr/bin/env bash
# Convertit toutes les ROMs .gb/.gbc d'un dossier en firmwares Gamebuino META.
#
# Usage : ./compile.sh [dossier_roms] [dossier_sortie_binaires]
#   Par défaut : toutes les ROMs de ./roms/gb/, binaires dans ./roms/bin/
#   (le .elf de débogage reste dans out/<jeu>/.pio/build/meta/)
#   MAX_SIZE=...  taille de ROM maximale en octets (défaut 0 = pas de limite ;
#                 une ROM qui ne tient pas dans la flash échoue au link)
#   PLAY=1        ouvre aussi une fenêtre pour jouer soi-même (défaut 0 : non)
#   AUTO=0        pas de partie automatique.  Par défaut, chaque ROM est jouée
#                 sans fenêtre ni son, avec des touches aléatoires (Start de
#                 temps en temps) : AUTO_SEEDS parties en parallèle (8 : une
#                 de 30 min de jeu, sept de 10 min, environ 11 s en tout),
#                 pour trouver le code chaud à compiler.
#   JOBS=n        ROMs traitées en parallèle (défaut : cœurs / 4)
#   AUTO_REDO=1   refaire la partie automatique même si elle est déjà enregistrée
#   REPLAY=1      rejouer les parties à la main enregistrées (profils à jour)
#   STATIC=0      ne pas ajouter le code trouvé par analyse statique
#                 (par défaut il complète les parties, dans la limite de la flash)
#
# Sans aucune partie à la main, la partie automatique et l'analyse statique
# suffisent à compiler l'essentiel ; le code est gardé du plus exécuté au
# moins exécuté jusqu'à remplir la flash.
#
# ROMs de 256 Kio et plus : elles ne tiennent pas dans la flash.  Le
# firmware garde la banque 0 et les données les plus lues, et lit le reste
# sur la carte SD ; le script copie la ROM à côté du binaire sous un nom
# court (ex. MYSTICQU.GB), à mettre sur la carte avec le .bin.
#
# Jeux à pile (sauvegarde) ou à grande RAM de cartouche : le script crée un
# fichier .SAV (ex. ZERDNODE.SAV) à copier une fois sur la carte ; la
# console y garde la progression.
#
# Chaque partie (aléatoire ou à la main) est sauvée dans
# <dossier_roms>/profiles/<sha1>/ : entrées (.input), code exécuté (.cov) et
# lectures de données (.prof).  Les lancements suivants les réutilisent au
# lieu de rejouer, et toutes sont cumulées : rejouer un jeu à la main étend
# le code compilé.
# Une ROM en échec n'arrête pas le lot ; le bilan est affiché à la fin.
set -uo pipefail

SRC_DIR="${1:-./roms/gb}"
DEST_BIN_DIR="${2:-./roms/bin}"
MAX_SIZE="${MAX_SIZE:-0}"
PLAY="${PLAY:-0}"
STATIC="${STATIC:-1}"
AUTO="${AUTO:-1}"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# Précalcul : AUTO_SEEDS parties aléatoires en parallèle par ROM, une de
# AUTO_LONG_FRAMES (30 min de jeu) et les autres de AUTO_FRAMES (10 min).
# Mesuré sur 4 jeux : au-delà, moins de 5 % de code en plus ; environ 11 s
# par ROM sur 8 cœurs.
AUTO_SEEDS="${AUTO_SEEDS:-8}"
AUTO_FRAMES="${AUTO_FRAMES:-36000}"
AUTO_LONG_FRAMES="${AUTO_LONG_FRAMES:-108000}"
AUTO_SECONDS="${AUTO_SECONDS:-60}"  # plafond de temps réel par partie
# ROMs traitées en parallèle (1 avec PLAY=1) ; journal de chacune dans
# out/<jeu>/compile.log.
JOBS="${JOBS:-$(( NPROC / 4 > 0 ? NPROC / 4 : 1 ))}"
AUTO_REDO="${AUTO_REDO:-0}"
REPLAY="${REPLAY:-0}"
GBRECOMP="./build/bin/gbrecomp"
# Profils fournis (couverture jouée, lectures de données), rangés par SHA-1
# de la ROM : un dump identique en profite sans rejouer.
PROFILES_DIR="${PROFILES_DIR:-$(dirname "$0")/profiles}"
# Parties enregistrées par ce script (aléatoires et à la main), gardées d'un
# lancement à l'autre : <dossier_roms>/profiles/<sha1>/.
SESSIONS_DIR="${SESSIONS_DIR:-$SRC_DIR/profiles}"

if [ ! -x "$GBRECOMP" ]; then
  echo "gbrecomp introuvable : compile d'abord l'outil (cmake -G Ninja -B build . && ninja -C build)."
  exit 1
fi
mkdir -p "$DEST_BIN_DIR"

shopt -s nullglob nocaseglob
roms=("$SRC_DIR"/*.gb "$SRC_DIR"/*.gbc)
shopt -u nullglob nocaseglob

if [ ${#roms[@]} -eq 0 ]; then
  echo "Aucun fichier .gb ou .gbc trouvé dans '$SRC_DIR'."
  exit 0
fi


convert() {
  local rom_path="$1" prefix="$2"
  local out_dir="out/$prefix"

  # a0) Parties enregistrées pour cette ROM exacte (par SHA-1) : celles du
  #     dossier profiles/ fourni, puis celles sauvées par les lancements
  #     précédents dans $SESSIONS_DIR/<sha1>/ (source de vérité).
  local sha f store
  sha=$( (sha1sum "$rom_path" 2>/dev/null || shasum -a 1 "$rom_path") | cut -d' ' -f1)
  store="$SESSIONS_DIR/$sha"
  mkdir -p "$out_dir" "$store"
  [ -f "$store/JEU.txt" ] || basename "$rom_path" > "$store/JEU.txt"
  save_sessions() { # out/<jeu> -> dossier des parties (sans écraser)
    for f in "$out_dir"/coverage-*.cov "$out_dir"/coverage-*.input "$out_dir"/romdata-*.prof; do
      [ -f "$f" ] && [ ! -f "$store/$(basename "$f")" ] && cp "$f" "$store/"
    done
    [ -f "$out_dir/auto.input" ] && [ ! -f "$store/coverage-auto.input" ] && cp "$out_dir/auto.input" "$store/coverage-auto.input"
    return 0
  }
  save_sessions # parties laissées dans out/ par une version précédente du script
  if [ -d "$PROFILES_DIR/$sha" ]; then
    for f in "$PROFILES_DIR/$sha"/*.cov "$PROFILES_DIR/$sha"/*.prof; do
      [ -f "$f" ] && [ ! -f "$out_dir/$(basename "$f")" ] && cp "$f" "$out_dir/"
    done
    echo "[i] Profils fournis utilisés : $(cat "$PROFILES_DIR/$sha/JEU.txt" 2>/dev/null || echo "$sha")"
  fi
  for f in "$store"/coverage-*.cov "$store"/romdata-*.prof; do
    [ -f "$f" ] && cp "$f" "$out_dir/"
  done
  local n_sessions
  n_sessions=$(ls "$store"/coverage-*.cov 2>/dev/null | wc -l)
  [ "$n_sessions" -gt 0 ] && echo "[i] $n_sessions partie(s) déjà enregistrée(s) réutilisée(s) : $store"

  # a) Projet interpréteur seul et outils PC
  "$GBRECOMP" "$rom_path" --target meta --output-prefix "$prefix" -o "$out_dir" || return 1
  cmake -G Ninja -S "$out_dir/host" -B "$out_dir/host/build" > /dev/null || return 1
  ninja -C "$out_dir/host/build" || return 1

  # a2) REPLAY=1 : rejoue les parties à la main enregistrées (utile après une
  #     mise à jour du runtime ou pour ajouter le profil de lectures SD).
  if [ "$REPLAY" = "1" ]; then
    local inp id last
    for inp in "$store"/coverage-*.input; do
      [ -f "$inp" ] || continue
      id=$(basename "$inp" .input); id=${id#coverage-}
      case "$id" in auto*) continue ;; esac
      last=$(awk 'END { print $1 + 120 }' "$inp")
      echo "[i] Partie $id rejouée ($last frames)"
      "$out_dir/host/build/${prefix}_profile" --interp --frames "$last" --input "$inp" \
        --coverage "$store/coverage-$id.cov" --rom-profile-out "$store/romdata-$id.prof" > /dev/null || true
      cp "$store/coverage-$id.cov" "$out_dir/"
      [ -f "$store/romdata-$id.prof" ] && cp "$store/romdata-$id.prof" "$out_dir/"
    done
  fi

  # b) Partie interactive : enregistre une couverture de plus
  if [ "$PLAY" = "1" ]; then
    local stamp
    stamp=$(date +%Y%m%d-%H%M%S)
    echo "[!] Partie d'enregistrement. Flèches | X=A | Z=B | Entrée=Start | Retour=Select | Échap=Quitter"
    "$out_dir/host/build/${prefix}_play" --interp \
      --coverage "$out_dir/coverage-$stamp.cov" \
      --rom-profile-out "$out_dir/romdata-$stamp.prof" \
      --record-input "$out_dir/coverage-$stamp.input" || true
    save_sessions
  fi

  # b2) Parties automatiques, sans fenêtre ni son, en parallèle : Start de
  #     temps en temps, directions/A/B au hasard, une graine par partie.
  #     Déjà enregistrées pour cette ROM : réutilisées (AUTO_REDO=1 pour
  #     les refaire).
  if [ "$AUTO" = "1" ] && [ "$AUTO_REDO" != "1" ] && { compgen -G "$store/coverage-auto*.cov" > /dev/null ||
       compgen -G "$PROFILES_DIR/$sha/coverage-auto*.cov" > /dev/null; }; then
    echo "[i] Parties automatiques déjà enregistrées : réutilisées"
  elif [ "$AUTO" = "1" ]; then
    rm -f "$store"/coverage-auto* "$store"/romdata-auto* "$out_dir"/coverage-auto* "$out_dir"/romdata-auto* "$out_dir/auto.input"
    local guard=() seed frames
    command -v timeout > /dev/null && guard=(timeout $((AUTO_SECONDS + 30)))
    for seed in $(seq 1 "$AUTO_SEEDS"); do
      frames=$AUTO_FRAMES
      [ "$seed" = 1 ] && frames=$AUTO_LONG_FRAMES
      awk -v frames="$frames" -v seed=$((41 + seed)) 'BEGIN { srand(seed); split("right left up down a b", n, " ");
        for (f = 30; f < frames; f += 20) {
          if (f % 600 < 20) { print f, "start"; continue }
          b = ""; for (i = 1; i <= 6; i++) if (rand() < (i == 1 ? 0.55 : 0.25)) b = b (b == "" ? "" : "+") n[i];
          print f, (b == "" ? "none" : b) } }' > "$out_dir/coverage-auto$seed.input"
      "${guard[@]}" "$out_dir/host/build/${prefix}_profile" --interp --frames "$frames" \
        --max-seconds "$AUTO_SECONDS" --input "$out_dir/coverage-auto$seed.input" \
        --coverage "$out_dir/coverage-auto$seed.cov" \
        --rom-profile-out "$out_dir/romdata-auto$seed.prof" > /dev/null 2>&1 &
    done
    wait
    save_sessions
  fi

  # c) Régénération avec toutes les couvertures enregistrées (+ analyse statique)
  local cov_args=()
  local cov
  for cov in "$out_dir"/coverage-*.cov; do
    [ -f "$cov" ] && cov_args+=(--coverage "$cov")
  done
  # Lectures de données dans la ROM (jeux lus depuis la carte SD) : choisit
  # ce qui reste en flash.
  local prof
  for prof in "$out_dir"/romdata-*.prof; do
    [ -f "$prof" ] && cov_args+=(--rom-profile "$prof")
  done
  local static_args=()
  [ "$STATIC" = "1" ] && static_args=(--meta-static)
  if [ ${#cov_args[@]} -eq 0 ] && [ ${#static_args[@]} -eq 0 ]; then
    echo "[!] Aucune couverture : firmware 100 % interpréteur (plus lent)."
  fi

  # d) Firmware, en remplissant la flash au maximum.  Le code est trié du
  #    plus exécuté au moins exécuté ; --meta-bytes-per-instruction règle
  #    combien en garder.  Recherche dichotomique de la plus petite valeur
  #    (= le plus de code) qui tient encore dans les 240 Kio de la META.
  build_with() { # $1 = octets par instruction ; 0 ok, 1 flash pleine, 2 erreur
    gen_log=$("$GBRECOMP" "$rom_path" --target meta --output-prefix "$prefix" \
      "${cov_args[@]}" "${static_args[@]}" --meta-bytes-per-instruction "$1" -o "$out_dir") || return 2
    build_log=$(pio run -d "$out_dir" 2>&1) && return 0
    echo "$build_log" | grep -q -E "overflowed|greater than maximum allowed" && return 1
    echo "$build_log" | tail -20
    return 2
  }
  local lo=8 hi=64 mid best=0 rc build_log gen_log
  if [ ${#static_args[@]} -eq 0 ] && [ ${#cov_args[@]} -eq 0 ]; then lo=64; fi
  while [ "$lo" -le "$hi" ]; do
    mid=$(( (lo + hi) / 2 ))
    build_with "$mid"; rc=$?
    if [ $rc -eq 0 ]; then best=$mid; hi=$(( mid - 1 ))
    elif [ $rc -eq 1 ]; then lo=$(( mid + 1 ))
    else return 1
    fi
  done
  [ "$best" -gt 0 ] || { echo "[x] Même sans code compilé, le firmware ne tient pas en flash."; return 1; }
  # Le dernier essai n'est pas forcément le meilleur : reconstruire celui-ci.
  build_with "$best" || return 1
  echo "$gen_log" | grep -E "compiled:|interpreter only:|wait loops:|ROM streaming:"
  echo "$build_log" | grep -E "RAM:|Flash:"
  cp "$out_dir/.pio/build/meta/firmware.bin" "$DEST_BIN_DIR/${prefix}.bin"
  # Sauvegarde (cartouche à pile) ou grande RAM de cartouche : un fichier
  # .SAV de la taille de cette RAM, à mettre sur la carte à côté du .bin.
  # Jamais écrasé : il contient la progression.
  local save_name eram battery
  save_name=$(sed -n 's/^#define GBL_SAVE_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  eram=$(sed -n 's/^#define GBL_ERAM_SIZE \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  battery=$(grep -c '^#define GBL_BATTERY 1' "$out_dir/src/gbl_config.h")
  if [ -n "$save_name" ] && { [ "$battery" = "1" ] || [ "${eram:-0}" -gt 2048 ]; }; then
    if [ ! -f "$DEST_BIN_DIR/$save_name" ]; then
      head -c "$eram" /dev/zero | tr '\0' '\377' > "$DEST_BIN_DIR/$save_name"
    fi
    echo "[i] Sauvegarde : copie $DEST_BIN_DIR/$save_name sur la carte avec le .bin, UNE SEULE FOIS"
    echo "    (ne l'écrase plus ensuite : la console y écrit la progression)."
  fi
  # ROM trop grosse pour la flash : le firmware la lit sur la carte SD.
  local stream_name
  stream_name=$(sed -n 's/^#define GBL_STREAM_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  if [ -n "$stream_name" ]; then
    cp "$rom_path" "$DEST_BIN_DIR/$stream_name"
    echo "[i] ROM lue depuis la carte SD : copie aussi $DEST_BIN_DIR/$stream_name sur la carte"
    echo "    (à la racine ou dans le dossier du jeu, sous ce nom exact)."
  fi
}

[ "$PLAY" = "1" ] && JOBS=1
STATUS_DIR=$(mktemp -d)
trap 'rm -rf "$STATUS_DIR"' EXIT
total=${#roms[@]}
echo "${total} ROM(s), ${JOBS} en parallèle, ${AUTO_SEEDS} parties automatiques par ROM (${NPROC} cœurs)"

run_one() { # $1 rom, $2 préfixe, $3 nom de fichier
  if convert "$1" "$2"; then
    echo "$2" > "$STATUS_DIR/ok.$2"
  else
    echo "$3" > "$STATUS_DIR/fail.$2"
    return 1
  fi
}

running=0
for rom_path in "${roms[@]}"; do
  size=$(stat -c%s "$rom_path" 2>/dev/null || stat -f%z "$rom_path")
  filename=$(basename "$rom_path")
  if [ "$MAX_SIZE" -gt 0 ] && [ "$size" -gt "$MAX_SIZE" ]; then
    echo "[-] Ignoré (ROM > $MAX_SIZE octets) : $filename ($size octets)"
    echo "$filename" > "$STATUS_DIR/skip.$filename"
    continue
  fi

  # Identifiant C : caractères spéciaux -> '_', jamais de chiffre en tête.
  prefix=$(printf '%s' "${filename%.*}" | tr -c 'a-zA-Z0-9_' '_')
  case "$prefix" in [0-9]*) prefix="g$prefix" ;; esac

  if [ "$JOBS" -le 1 ]; then
    echo "=========================================="
    echo "[+] $filename (préfixe : $prefix)"
    echo "=========================================="
    if run_one "$rom_path" "$prefix" "$filename"; then
      echo "[+] Terminé : $DEST_BIN_DIR/$prefix.bin"
    else
      echo "[x] Échec pour $filename (ROM refusée ou firmware trop gros : voir le message ci-dessus)"
    fi
    echo ""
  else
    mkdir -p "out/$prefix"
    (
      if run_one "$rom_path" "$prefix" "$filename" > "out/$prefix/compile.log" 2>&1; then
        echo "[+] $filename"
      else
        echo "[x] $filename (voir out/$prefix/compile.log)"
      fi
    ) &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
      wait -n
      running=$((running - 1))
    fi
  fi
done
wait

n_ok=$(ls "$STATUS_DIR" | grep -c '^ok\.')
n_fail=$(ls "$STATUS_DIR" | grep -c '^fail\.')
n_skip=$(ls "$STATUS_DIR" | grep -c '^skip\.')
echo "------------------------------------------"
echo "Réussis : $n_ok   Échecs : $n_fail   Ignorés : $n_skip"
for f in "$STATUS_DIR"/fail.*; do [ -f "$f" ] && echo "  échec : $(cat "$f")"; done
echo "Binaires dans : $DEST_BIN_DIR"
extra=$(cd "$DEST_BIN_DIR" && ls *.GB *.SAV 2>/dev/null | tr '\n' ' ')
[ -n "$extra" ] && echo "À copier aussi sur la carte (à côté des .bin) : $extra  (.SAV une seule fois)"
[ "$n_fail" -eq 0 ]
