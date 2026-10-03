#!/usr/bin/env bash
# Convertit toutes les ROMs .gb/.gbc d'un dossier en jeux Gamebuino META
# (ou Pokitto, TARGET=pokitto), rangés comme le loader de la console les
# attend.
#
# Usage : ./compile.sh [dossier_roms] [dossier_sortie]
#   Par défaut : toutes les ROMs de ./roms/gb/, jeux dans ./roms/meta/<Jeu>/
#   (le .elf de débogage reste dans out/<jeu>/.pio/build/meta/)
#
#   TARGET=pokitto  même chaîne pour la Pokitto : jeux dans ./roms/pokitto/<Jeu>/,
#                   projets dans out/pokitto/<jeu>/ (build/firmware.elf), firmware
#                   construit par make (arm-none-eabi-gcc seul, sans PlatformIO),
#                   228 Kio de flash sous le loader au lieu de 240, pas
#                   d'icônes (format du loader META).  Les parties
#                   enregistrées sont communes aux deux consoles.
#
# Chaque jeu devient un dossier à copier tel quel à la racine de la carte SD :
#   <Jeu>/<Jeu>.bin        le firmware (nommé simplement : « Zerd (J) [!] »
#                          -> « Zerd » ; c'est ce nom que montre le loader,
#                          38 caractères au plus : voir loader_name)
#   <Jeu>/ICON.BMP         icône 32x32 du menu (grille)
#   <Jeu>/TITLESCREEN.BMP  écran titre 80x64 du menu (détail)
#     Icônes tirées de thumbnails.libretro.com (Named_Titles), mises en cache
#     dans <dossier_sortie>/.cache/icons/ ; un fichier local du même nom
#     (<dossier_roms>/icons/<Jeu>.png) a la priorité.  Un échec n'arrête
#     rien : le loader affiche son icône par défaut.  NO_ICONS=1 pour
#     désactiver les téléchargements.
#   <Jeu>/<NOM>.SAV        fichier de sauvegarde (cartouche à pile ou grande
#                          RAM) : créé une seule fois, la console y écrit.
#   <Jeu>/<NOM>.GB         la ROM entière, pour les ROMs qui ne tiennent pas
#                          dans la flash (256 Kio et plus) : le firmware
#                          garde banque 0 et données les plus lues en flash
#                          et lit le reste sur la carte.  Pas de limite de
#                          taille de ROM.
#   <Jeu>/.gbrmeta.json    état interne (ROM, sha1, version, PACK) ; ignorable.
#
# Versions : chaque firmware embarque la version de gbrecomp qui l'a produit
# (chaîne « gbrecomp-meta x.y.z » dans le .bin).  compile.sh la relit : un firmware déjà à jour (même
# version, même ROM) est conservé tel quel ; un firmware fait par une autre
# version est proposé à la mise à jour.
#
# Interface :
#   - Interactif quand le script est lancé dans un terminal : liste des ROMs
#     avec leur état (nouvelle, à jour, version vA -> vB, ROM modifiée,
#     version inconnue), choix des jeux à traiter, puis confirmation pour
#     reconstruire les firmwares d'une autre version.
#   - Sinon (script, tube) : traite tout ce qui a à faire.
#       ALL=1       comme si on répondait « tout » dans le menu
#       UPDATE=1    reconstruit aussi les firmwares d'une autre version
#       FORCE=1     refait même ceux déjà à jour
#
#   PLAY=1        ouvre aussi une fenêtre pour jouer soi-même (défaut 0 : non)
#   AUTO=1        (défaut) chaque ROM est jouée sans fenêtre ni son, avec des
#                 touches aléatoires (Start de temps en temps), à pleine
#                 vitesse : AUTO_SEEDS parties en parallèle (défaut : une par
#                 cœur) pendant AUTO_SECONDS secondes (défaut 20), pour
#                 trouver le code chaud à compiler.  Les parties automatiques
#                 de plusieurs ROMs passent une à une (elles prennent tous
#                 les cœurs) ; le reste du lot continue en parallèle.
#   JOBS=n        ROMs traitées en parallèle (défaut : cœurs / 4 ; 1 avec PLAY=1)
#   AUTO_REDO=1   refaire la partie automatique même si elle est déjà enregistrée
#                 (sinon seules les graines manquantes sont jouées)
#   REPLAY=1      rejouer les parties à la main enregistrées (profils à jour)
#   PACK=1 (défaut)
#                 données de ROM peu lues compressées (LZSS) dans la flash au
#                 lieu d'être lues sur la carte SD : plus de place pour le code
#                 compilé, pas de .GB à copier (retombe sur la SD ou le mode
#                 normal si la ROM compressée ne tient pas)
#   STATIC=0      ne pas ajouter le code trouvé par analyse statique
#                 (par défaut il complète les parties, dans la limite de la flash)
#
# Sans aucune partie à la main, la partie automatique et l'analyse statique
# suffisent à compiler l'essentiel ; le code est gardé du plus exécuté au
# moins exécuté jusqu'à remplir la flash.
#
# Jeux à pile (sauvegarde) ou à grande RAM de cartouche : le fichier .SAV
# (ex. ZERDNODE.SAV) est créé une fois ; la console y garde la progression.
#
# Chaque partie (aléatoire ou à la main) est sauvée dans
# <dossier_roms>/profiles/<sha1>/ : entrées (.input), code exécuté (.cov) et
# lectures de données (.prof).  Les lancements suivants les réutilisent au
# lieu de rejouer, et toutes sont cumulées : rejouer un jeu à la main étend
# le code compilé.
# Une ROM en échec n'arrête pas le lot ; le bilan est affiché à la fin.
set -uo pipefail

TARGET="${TARGET:-meta}"
case "$TARGET" in
  meta|pokitto) ;;
  *) echo "TARGET=$TARGET inconnu (meta ou pokitto)."; exit 1 ;;
esac
SRC_DIR="${1:-./roms/gb}"
DEST_DIR="${2:-./roms/$TARGET}"
# Projets générés : out/<jeu> pour la META, out/pokitto/<jeu> pour la Pokitto
# (les deux peuvent coexister).
if [ "$TARGET" = pokitto ]; then OUT_ROOT="out/pokitto"; else OUT_ROOT="out"; fi
PLAY="${PLAY:-0}"
STATIC="${STATIC:-1}"
AUTO="${AUTO:-1}"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# Précalcul : AUTO_SEEDS parties aléatoires en parallèle par ROM (une par
# cœur), chacune jouée à pleine vitesse pendant AUTO_SECONDS secondes de
# temps réel (3 000 à 12 000 frames par seconde selon le jeu).
AUTO_SEEDS="${AUTO_SEEDS:-$NPROC}"
AUTO_SECONDS="${AUTO_SECONDS:-20}"
# ROMs traitées en parallèle (1 avec PLAY=1) ; journal de chacune dans
# out/<jeu>/compile.log.
JOBS="${JOBS:-$(( NPROC / 4 > 0 ? NPROC / 4 : 1 ))}"
AUTO_REDO="${AUTO_REDO:-0}"
REPLAY="${REPLAY:-0}"
PACK="${PACK:-1}"  # auto : packe quand ça tient, retombe sur la carte SD sinon (PACK=0 pour désactiver)
ALL="${ALL:-0}"
UPDATE="${UPDATE:-0}"
FORCE="${FORCE:-0}"
NO_ICONS="${NO_ICONS:-0}"
GBRECOMP="./build/bin/gbrecomp"
# Profils fournis (couverture jouée, lectures de données), rangés par SHA-1
# de la ROM : un dump identique en profite sans rejouer.
PROFILES_DIR="${PROFILES_DIR:-$(dirname "$0")/profiles}"
# Parties enregistrées par ce script (aléatoires et à la main), gardées d'un
# lancement à l'autre : <dossier_roms>/profiles/<sha1>/.
SESSIONS_DIR="${SESSIONS_DIR:-$SRC_DIR/profiles}"
ICON_CACHE="$DEST_DIR/.cache/icons"
THUMB_BASE="https://thumbnails.libretro.com"

if [ ! -x "$GBRECOMP" ]; then
  echo "gbrecomp introuvable : compile d'abord l'outil (cmake -G Ninja -B build . && ninja -C build)."
  exit 1
fi
# Version de l'édition META (targets/meta/VERSION), celle inscrite dans les
# firmwares ; un gbrecomp plus ancien ne connaît que --version-json.
CUR_VERSION=$("$GBRECOMP" --meta-version 2>/dev/null | head -n1)
case "$CUR_VERSION" in
  [0-9]*) ;;
  *) CUR_VERSION=$("$GBRECOMP" --version-json | sed -n 's/.*"version":"\([^"]*\)".*/\1/p') ;;
esac
[ -n "$CUR_VERSION" ] || CUR_VERSION="inconnue"
mkdir -p "$DEST_DIR"

sha1_of() { (sha1sum "$1" 2>/dev/null || shasum -a 1 "$1") | cut -d' ' -f1; }

# « Zerd no Densetsu (J) [!] » -> « Zerd no Densetsu »
clean_name() {
  local n="${1%.*}"
  n=$(printf '%s' "$n" | sed -E 's/[[(][^])]*[])]//g; s/[[:space:]]+/ /g;
                                s/^[ ._-]+//; s/[ ._-]+$//')
  printf '%s' "${n:-${1%.*}}"
}

# Caractères interdits sur une carte FAT -> espace.
sd_safe() {
  printf '%s' "$1" | sed -E 's#[\\/:*?"<>|]# #g; s/[[:space:]]+/ /g; s/^ //; s/ $//'
}

# Le loader de la META range chaque dossier de jeu dans 40 octets
# (loader/GAMEFOLDERS.CACHE : « /Nom » et un zéro) : 38 octets au plus pour
# le nom, qui est aussi celui du .bin.  Plus long : l'article final remis
# devant (« Legend of Zelda, The - X » -> « The Legend of Zelda - X »), puis
# une coupe sur un mot (le début du sous-titre reste : il distingue les jeux
# d'une même série).
LOADER_NAME_MAX=38
loader_name() {
  local LC_ALL=C s="$1" max="${2:-$LOADER_NAME_MAX}"
  [ "${#s}" -le "$max" ] && { printf '%s' "$s"; return; }
  s=$(printf '%s' "$s" | sed -E 's/^([^,]+), (The|A|An|Le|La|Les|L'"'"'|Die|Der|Das) - /\2 \1 - /; s/^([^,]+), (The|A|An|Le|La|Les|Die|Der|Das)$/\2 \1/')
  if [ "${#s}" -gt "$max" ]; then
    s="${s:0:$max}"
    [[ "$s" == *" "* ]] && s="${s% *}"
    # Pas de mot de liaison pendant à la fin (« Asteroids & », « Bart vs. the »).
    s=$(printf '%s' "$s" | sed -E ':a; s/[ ,.:-]+$//; s/ (&|and|of|the|The|a|A|an|An|vs|vs\.|no|o|to|de|wa|ga|with|in)$//; ta')
  fi
  printf '%s' "$s" | sed -E 's/[ ,.:-]+$//'
}

url_quote() {
  python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1" \
    2>/dev/null || printf '%s' "$1" | sed 's/ /%20/g'
}

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

# Version de gbrecomp embarquée dans un firmware (voir meta_main.cpp) :
# « gbrecomp-meta x.y.z » ou « gbrecomp-pokitto x.y.z ».
firmware_version() {
  grep -aoE "gbrecomp-$TARGET [0-9A-Za-z._+-]{1,32}" "$1" 2>/dev/null \
    | head -n1 | cut -d' ' -f2
}

# Outil de conversion PNG -> BMP (le loader attend des BMP 24 bits).
IM_BIN=none
command -v magick > /dev/null 2>&1 && IM_BIN=magick
[ "$IM_BIN" = none ] && command -v convert > /dev/null 2>&1 && IM_BIN=convert
HAVE_PIL=0
python3 -c 'import PIL' > /dev/null 2>&1 && HAVE_PIL=1

make_bmp() { # $1 png, $2 bmp, $3 largeur, $4 hauteur
  if [ "$IM_BIN" != none ]; then
    "$IM_BIN" "$1" -thumbnail "$3x$4^" -gravity center -extent "$3x$4" \
      -strip -type truecolor "BMP3:$2" > /dev/null 2>&1
  elif [ "$HAVE_PIL" = 1 ]; then
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
from PIL import Image, ImageOps
im = Image.open(sys.argv[1]).convert("RGB")
ImageOps.fit(im, (int(sys.argv[3]), int(sys.argv[4]))).save(sys.argv[2], "BMP")
PY
  else
    return 1
  fi
}

# Nom (décodé <TAB> encodé) de chaque vignette d'un catalogue libretro,
# mis en cache : les fichiers Named_Titles gardent les tags No-Intro
# (« Tetris (World) (Rev 1).png »), un nom nettoyé seul ne suffit pas.
thumb_listing() { # $1 = gbc ou "" -> fichier cache, ou échec
  local f url
  if [ "$1" = gbc ]; then
    f="$ICON_CACHE/listing-gbc.txt"
    url="$THUMB_BASE/Nintendo%20-%20Game%20Boy%20Color/Named_Titles/"
  else
    f="$ICON_CACHE/listing-gb.txt"
    url="$THUMB_BASE/Nintendo%20-%20Game%20Boy/Named_Titles/"
  fi
  if [ ! -s "$f" ]; then
    mkdir -p "$ICON_CACHE"
    if command -v curl > /dev/null 2>&1; then
      curl -fsSL --max-time 60 -o "$f.raw" "$url" || { rm -f "$f.raw"; return 1; }
    elif command -v wget > /dev/null 2>&1; then
      wget -q -T 60 -O "$f.raw" "$url" || { rm -f "$f.raw"; return 1; }
    else
      return 1
    fi
    python3 - "$f.raw" "$f" <<'PY'
import re, sys, urllib.parse
raw, out = sys.argv[1], sys.argv[2]
text = open(raw, encoding="utf-8", errors="replace").read()
with open(out, "w", encoding="utf-8") as f:
    for enc in re.findall(r'href="([^"]+\.png)"', text):
        f.write(urllib.parse.unquote(enc)[:-4] + "\t" + enc[:-4] + "\n")
PY
    rm -f "$f.raw"
    [ -s "$f" ] || return 1
  fi
  printf '%s' "$f"
}

# Meilleure vignette pour un nom nettoyé : sans les tags (région, révision,
# [!]) ; (World) puis (USA, Europe) préférés, révision la plus haute.
find_thumb() { # $1 nom du jeu, $2 = gbc ou "" -> nom de fichier encodé
  local list
  list=$(thumb_listing "$2") || return 1
  python3 - "$list" "$1" <<'PY'
import re, sys
def norm(s):
    return " ".join(re.sub(r"\([^)]*\)|\[[^\]]*\]", " ", s).split()).casefold()
target = norm(sys.argv[2])
best, best_score = None, float("-inf")
for line in open(sys.argv[1], encoding="utf-8"):
    dec, _, enc = line.rstrip("\n").partition("\t")
    if norm(dec) != target:
        continue
    dl = dec.casefold()
    score = 0
    for tag, pts in (("(world)", 40), ("(usa, europe)", 30),
                     ("(usa)", 20), ("(europe)", 20)):
        if tag in dl:
            score += pts
    m = re.search(r"\(rev (\d+)\)", dl)
    if m:
        score += int(m.group(1))
    if "(japan)" in dl and "(en)" not in dl:
        score -= 10
    if score > best_score:
        best, best_score = enc, score
if best:
    print(best)
PY
}

fetch_icons() { # $1 dossier du jeu, $2 nom du jeu, $3 base du fichier ROM, $4 "gbc" ou ""
  [ "$NO_ICONS" = 1 ] && return 0
  local dir="$1" name="$2" base="$3" png="$ICON_CACHE/$2.png" cand set url enc
  if [ ! -f "$png" ]; then
    for cand in "$SRC_DIR/icons/$2.png" "$SRC_DIR/icons/$3.png" "$SRC_DIR/$3.png"; do
      if [ -f "$cand" ]; then mkdir -p "$ICON_CACHE"; cp "$cand" "$png"; break; fi
    done
  fi
  if [ ! -f "$png" ]; then
    # Les jeux Game Boy Color ont leur propre catalogue libretro.
    local sets=("$4" "")
    [ "$4" = gbc ] && sets=("" gbc)
    mkdir -p "$ICON_CACHE"
    for set in "${sets[@]}"; do
      enc=$(find_thumb "$2" "$set") && [ -n "$enc" ] && break
    done
    if [ -z "${enc:-}" ]; then
      # Catalogue inaccessible ou nom absent : essais directs en dernier
      # recours (nom nettoyé supposé identique au nom No-Intro).
      for set in "Nintendo%20-%20Game%20Boy" "Nintendo%20-%20Game%20Boy%20Color"; do
        url="$THUMB_BASE/$set/Named_Titles/$(url_quote "$2").png"
        if curl -fsSL --max-time 30 -o "$png" "$url" 2>/dev/null; then break; fi
        if wget -q -T 30 -O "$png" "$url" 2>/dev/null; then break; fi
        rm -f "$png"
      done
    else
      if [ "$set" = gbc ]; then set="Nintendo%20-%20Game%20Boy%20Color"
      else set="Nintendo%20-%20Game%20Boy"; fi
      url="$THUMB_BASE/$set/Named_Titles/$enc.png"
      curl -fsSL --max-time 30 -o "$png" "$url" 2>/dev/null || \
        wget -q -T 30 -O "$png" "$url" 2>/dev/null || rm -f "$png"
    fi
  fi
  if [ ! -f "$png" ]; then
    echo "[!] Icône introuvable pour « $2 » sur thumbnails.libretro.com"
    echo "    (nom No-Intro différent ? dépose $SRC_DIR/icons/$2.png)"
    return 1
  fi
  if ! make_bmp "$png" "$dir/TITLESCREEN.BMP" 80 64 || \
     ! make_bmp "$png" "$dir/ICON.BMP" 32 32; then
    echo "[!] Conversion d'icône impossible pour « $2 » (ImageMagick ou PIL requis)"
    rm -f "$dir/TITLESCREEN.BMP" "$dir/ICON.BMP"
    return 1
  fi
  return 0
}

# État d'un jeu : chaîne lisible pour le menu + décision de (re)construction.
# Entrées : $1 dossier du jeu, $2 sha1 de la ROM.  Sorties globales :
# S_tag (affichage), S_todo (0 rien, 1 à faire, 2 à confirmer).
assess() {
  local dir="$1" sha="$2"
  local bin="$DEST_DIR/$dir/$dir.bin"
  local state="$DEST_DIR/$dir/.gbrmeta.json"
  local ver="" state_sha="" state_pack=""
  S_todo=1; S_tag="nouvelle"
  if [ -f "$bin" ]; then
    ver=$(firmware_version "$bin")
    if [ -z "$ver" ] && [ -f "$state" ]; then
      ver=$(sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$state")
    fi
    [ -f "$state" ] && state_sha=$(sed -n 's/.*"sha1":[[:space:]]*"\([^"]*\)".*/\1/p' "$state")
    [ -f "$state" ] && state_pack=$(sed -n 's/.*"pack":[[:space:]]*\([01]\).*/\1/p' "$state")
    if [ -n "$state_sha" ] && [ "$state_sha" != "$sha" ]; then
      S_tag="ROM modifiée"; S_todo=1
    elif [ "${state_pack:-0}" != "$PACK" ]; then
      S_tag="PACK=${state_pack:-0} -> PACK=$PACK"; S_todo=1
    elif [ -z "$ver" ]; then
      S_tag="version inconnue"; S_todo=2
    elif [ "$ver" = "$CUR_VERSION" ]; then
      S_tag="à jour (v$CUR_VERSION)"; S_todo=0
    else
      S_tag="v$ver -> v$CUR_VERSION"; S_todo=2
    fi
  fi
  return 0
}

in_array() { # $1 valeur, $2... tableau
  local v="$1"; shift
  local x
  for x in "$@"; do [ "$x" = "$v" ] && return 0; done
  return 1
}


build_game() { # $1 rom, $2 préfixe, $3 dossier du jeu (dernier composant), $4 sha1
  local rom_path="$1" prefix="$2" game_name="$3" rom_sha="$4"
  local game_dir="$DEST_DIR/$game_name"
  local out_dir="$OUT_ROOT/$prefix"

  # a0) Parties enregistrées pour cette ROM exacte (par SHA-1) : celles du
  #     dossier profiles/ fourni, puis celles sauvées par les lancements
  #     précédents dans $SESSIONS_DIR/<sha1>/ (source de vérité).
  local f store gbc=""
  case "${rom_path##*.}" in gbc|GBC) gbc=gbc ;; esac
  store="$SESSIONS_DIR/$rom_sha"
  mkdir -p "$out_dir" "$store" "$game_dir"
  [ -f "$store/JEU.txt" ] || basename "$rom_path" > "$store/JEU.txt"
  save_sessions() { # out/<jeu> -> dossier des parties (sans écraser)
    for f in "$out_dir"/coverage-*.cov "$out_dir"/coverage-*.input "$out_dir"/romdata-*.prof; do
      [ -f "$f" ] && [ ! -f "$store/$(basename "$f")" ] && cp "$f" "$store/"
    done
    [ -f "$out_dir/auto.input" ] && [ ! -f "$store/coverage-auto.input" ] && cp "$out_dir/auto.input" "$store/coverage-auto.input"
    return 0
  }
  save_sessions # parties laissées dans out/ par une version précédente du script
  if [ -d "$PROFILES_DIR/$rom_sha" ]; then
    for f in "$PROFILES_DIR/$rom_sha"/*.cov "$PROFILES_DIR/$rom_sha"/*.prof; do
      [ -f "$f" ] && [ ! -f "$out_dir/$(basename "$f")" ] && cp "$f" "$out_dir/"
    done
    echo "[i] Profils fournis utilisés : $(cat "$PROFILES_DIR/$rom_sha/JEU.txt" 2>/dev/null || echo "$rom_sha")"
  fi
  for f in "$store"/coverage-*.cov "$store"/romdata-*.prof; do
    [ -f "$f" ] && cp "$f" "$out_dir/"
  done
  local n_sessions
  n_sessions=$(ls "$store"/coverage-*.cov 2>/dev/null | wc -l)
  [ "$n_sessions" -gt 0 ] && echo "[i] $n_sessions partie(s) déjà enregistrée(s) réutilisée(s) : $store"

  # a) Projet interpréteur seul et outils PC
  local pack_args=()
  [ "$PACK" = "1" ] && pack_args=(--meta-pack)
  "$GBRECOMP" "$rom_path" --target "$TARGET" --output-prefix "$prefix" "${pack_args[@]}" -o "$out_dir" || return 1
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
  #     les refaire).  Les graines manquantes sont ajoutées : les ROMs
  #     passées par une version qui ne jouait qu'une partie (coverage-auto,
  #     gardée) reçoivent les huit (une seule partie compilait du code moins
  #     général : Faceball 2000 -10 % par frame sur une partie inédite).
  local seed n_auto=0
  for seed in $(seq 1 "$AUTO_SEEDS"); do
    if [ -f "$store/coverage-auto$seed.cov" ] || [ -f "$PROFILES_DIR/$rom_sha/coverage-auto$seed.cov" ]; then
      n_auto=$((n_auto + 1))
    fi
  done
  if [ "$AUTO" = "1" ] && [ "$AUTO_REDO" != "1" ] && [ "$n_auto" -ge "$AUTO_SEEDS" ]; then
    echo "[i] Parties automatiques déjà enregistrées : réutilisées"
  elif [ "$AUTO" = "1" ]; then
    if [ "$AUTO_REDO" = "1" ]; then
      rm -f "$store"/coverage-auto* "$store"/romdata-auto* "$out_dir"/coverage-auto* "$out_dir"/romdata-auto* "$out_dir/auto.input"
    elif [ "$n_auto" -gt 0 ] || [ -f "$store/coverage-auto.cov" ]; then
      echo "[i] Parties automatiques : $n_auto sur $AUTO_SEEDS enregistrées, ajout des autres"
    fi
    local frames=$((AUTO_SECONDS * 40000)) lock=()
    # Une ROM à la fois sur tous les cœurs (flock, quand il existe).
    command -v flock > /dev/null && lock=(flock "$DEST_DIR/.gbrecomp-auto.lock")
    for seed in $(seq 1 "$AUTO_SEEDS"); do
      [ -f "$store/coverage-auto$seed.cov" ] && continue
      [ -f "$PROFILES_DIR/$rom_sha/coverage-auto$seed.cov" ] && continue
      awk -v frames="$frames" -v seed=$((41 + seed)) 'BEGIN { srand(seed); split("right left up down a b", n, " ");
        for (f = 30; f < frames; f += 20) {
          if (f % 600 < 20) { print f, "start"; continue }
          b = ""; for (i = 1; i <= 6; i++) if (rand() < (i == 1 ? 0.55 : 0.25)) b = b (b == "" ? "" : "+") n[i];
          print f, (b == "" ? "none" : b) } }' > "$out_dir/coverage-auto$seed.input"
    done
    # Pleine vitesse, arrêt au bout de AUTO_SECONDS ; l'entrée enregistrée
    # est coupée à la dernière frame jouée, pour rejouer la même partie.
    "${lock[@]}" bash -c '
      prof=$1 dir=$2 secs=$3 frames=$4; shift 4
      guard=(); command -v timeout > /dev/null && guard=(timeout $((secs + 30)))
      for seed in "$@"; do
        [ -f "$dir/coverage-auto$seed.input" ] && [ ! -f "$dir/coverage-auto$seed.cov" ] || continue
        ( n=$("${guard[@]}" "$prof" --interp --frames "$frames" --max-seconds "$secs" \
              --input "$dir/coverage-auto$seed.input" --coverage "$dir/coverage-auto$seed.cov" \
              --rom-profile-out "$dir/romdata-auto$seed.prof" 2> /dev/null | sed -n "s/^frames \([0-9]*\).*/\1/p")
          [ -n "$n" ] && awk -v n="$n" "\$1 <= n" "$dir/coverage-auto$seed.input" > "$dir/coverage-auto$seed.tmp" &&
            mv "$dir/coverage-auto$seed.tmp" "$dir/coverage-auto$seed.input" ) &
      done
      wait' _ "$out_dir/host/build/${prefix}_profile" "$out_dir" "$AUTO_SECONDS" "$frames" $(seq 1 "$AUTO_SEEDS")
    save_sessions
  fi

  # b3) Jeux lus depuis la carte SD ou compressés (PACK=1) : il faut savoir
  #     quelles données de la ROM sont lues pour choisir ce qui reste brut en
  #     flash.  Parties réutilisées sans ce profil (enregistrées avec un
  #     firmware ordinaire) : rejouées une fois pour le mesurer.
  if compgen -G "$out_dir/host/*_romfull.c" > /dev/null && ! compgen -G "$out_dir/romdata-*.prof" > /dev/null; then
    local inp id last guard=()
    command -v timeout > /dev/null && guard=(timeout $((AUTO_SECONDS + 30)))
    for inp in "$store"/coverage-*.input "$PROFILES_DIR/$rom_sha"/coverage-*.input; do
      [ -f "$inp" ] || continue
      id=$(basename "$inp" .input); id=${id#coverage-}
      [ -f "$out_dir/romdata-$id.prof" ] && continue
      last=$(awk 'END { print $1 + 120 }' "$inp")
      "${guard[@]}" "$out_dir/host/build/${prefix}_profile" --interp --frames "$last" \
        --max-seconds "$AUTO_SECONDS" --input "$inp" \
        --rom-profile-out "$out_dir/romdata-$id.prof" > /dev/null 2>&1 &
    done
    wait
    compgen -G "$out_dir/romdata-*.prof" > /dev/null &&
      echo "[i] Profil des lectures de ROM mesuré en rejouant les parties enregistrées"
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
  #    (= le plus de code) qui tient encore dans les 240 Kio de la META
  #    (228 Kio sur la Pokitto).
  build_with() { # $1 = octets par instruction ; 0 ok, 1 flash pleine, 2 erreur
    gen_log=$("$GBRECOMP" "$rom_path" --target "$TARGET" --output-prefix "$prefix" \
      "${cov_args[@]}" "${static_args[@]}" "${pack_args[@]}" --meta-bytes-per-instruction "$1" -o "$out_dir") || return 2
    if [ "$TARGET" = pokitto ]; then
      build_log=$(make -C "$out_dir" -j"$NPROC" 2>&1) && return 0
    else
      build_log=$(pio run -d "$out_dir" 2>&1) && return 0
    fi
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
  echo "$gen_log" | grep -E "compiled:|interpreter only:|wait loops:|ROM streaming:|ROM packed:|--meta-pack:"
  if [ "$TARGET" = pokitto ]; then
    echo "$build_log" | grep -E "^flash:"
    cp "$out_dir/build/firmware.bin" "$game_dir/$game_name.bin"
  else
    echo "$build_log" | grep -E "RAM:|Flash:"
    cp "$out_dir/.pio/build/meta/firmware.bin" "$game_dir/$game_name.bin"
    # Icônes du loader (PNG libretro -> BMP 24 bits), sans bloquer le jeu.
    fetch_icons "$game_dir" "$game_name" "$(basename "$rom_path")" "$gbc" \
      || echo "[i] (jeu conservé sans icône)"
  fi
  # Sauvegarde (cartouche à pile) ou grande RAM de cartouche : un fichier
  # .SAV de la taille de cette RAM, à mettre sur la carte à côté du .bin.
  # Jamais écrasé : il contient la progression.
  local save_name eram battery
  save_name=$(sed -n 's/^#define GBL_SAVE_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  eram=$(sed -n 's/^#define GBL_ERAM_SIZE \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  battery=$(grep -c '^#define GBL_BATTERY 1' "$out_dir/src/gbl_config.h")
  if [ -n "$save_name" ] && { [ "$battery" = "1" ] || [ "${eram:-0}" -gt 2048 ]; }; then
    if [ ! -f "$game_dir/$save_name" ]; then
      head -c "$eram" /dev/zero | tr '\0' '\377' > "$game_dir/$save_name"
    fi
    echo "[i] Sauvegarde : $game_name/$save_name (déjà sur la carte : ne plus y toucher)"
  fi
  # États sauvegardés : un fichier de GBL_STATE_SLOTS emplacements, créé une
  # fois (jamais écrasé : il contient les états) ; sans lui, le menu de la
  # console n'offre pas Sauver/Charger l'état.
  local state_name state_slots state_sectors
  state_name=$(sed -n 's/^#define GBL_STATE_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  state_slots=$(sed -n 's/^#define GBL_STATE_SLOTS \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  state_sectors=$(sed -n 's/^#define GBL_STATE_SLOT_SECTORS \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  if [ -n "$state_name" ] && [ -n "$state_slots" ] && [ -n "$state_sectors" ]; then
    local state_bytes=$((state_slots * state_sectors * 512))
    if [ ! -f "$game_dir/$state_name" ] || [ "$(stat -c %s "$game_dir/$state_name")" -lt "$state_bytes" ]; then
      head -c "$state_bytes" /dev/zero > "$game_dir/$state_name"
    fi
    echo "[i] États : $game_name/$state_name ($state_slots emplacements, $((state_bytes / 1024)) Kio)"
  fi
  # ROM trop grosse pour la flash : le firmware la lit sur la carte SD.
  local stream_name
  stream_name=$(sed -n 's/^#define GBL_STREAM_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  if [ -n "$stream_name" ]; then
    cp "$rom_path" "$game_dir/$stream_name"
    echo "[i] ROM lue depuis la carte SD : $game_name/$stream_name (racine ou un dossier)"
  fi
  # État pour les lancements suivants : version, ROM exacte, fichiers.
  {
    printf '{\n  "rom": "%s",\n  "sha1": "%s",\n  "version": "%s",\n  "prefix": "%s",\n  "pack": %s,\n  "date": "%s"\n}\n' \
      "$(json_escape "$(basename "$rom_path")")" "$rom_sha" "$CUR_VERSION" "$prefix" "$PACK" "$(date +%Y-%m-%d)"
  } > "$game_dir/.gbrmeta.json"
  local fw_ver
  fw_ver=$(firmware_version "$game_dir/$game_name.bin")
  [ -n "$fw_ver" ] || fw_ver="$CUR_VERSION (pas de marqueur : gbrecomp trop ancien)"
  echo "[i] Firmware produit par gbrecomp v$fw_ver"
}


shopt -s nullglob nocaseglob
roms=("$SRC_DIR"/*.gb "$SRC_DIR"/*.gbc)
shopt -u nullglob nocaseglob

if [ ${#roms[@]} -eq 0 ]; then
  echo "Aucun fichier .gb ou .gbc trouvé dans '$SRC_DIR'."
  exit 0
fi

# ---- Inventaire : nom propre, préfixe C, sha1, état -----------------------

declare -a R_path R_base R_clean R_prefix R_sha R_status R_ver
declare -a used_prefixes=() used_dirs=() used_shas=()
n=0
n_dup=0
for rom_path in "${roms[@]}"; do
  sha=$(sha1_of "$rom_path")
  dup=0
  for ((i = 0; i < n; i++)); do
    [ "${R_sha[i]}" = "$sha" ] && { dup=1; break; }
  done
  if [ "$dup" = 1 ]; then
    echo "[-] Doublon (même sha1 que ${R_clean[i]}) : $(basename "$rom_path")"
    n_dup=$((n_dup + 1))
    continue
  fi

  long=$(sd_safe "$(clean_name "$(basename "$rom_path")")")
  [ -n "$long" ] || long=$(sd_safe "$(basename "$rom_path")")
  clean=$(loader_name "$long")
  dir="$clean"
  for ((k = 2; ; k++)); do
    busy=0
    for d in ${used_dirs[@]+"${used_dirs[@]}"}; do [ "$d" = "$dir" ] && busy=1; done
    [ "$busy" = 0 ] && break
    dir="$(loader_name "$clean" $((LOADER_NAME_MAX - 1 - ${#k}))) $k"
  done

  # Identifiant C : caractères spéciaux -> '_', jamais de chiffre en tête.
  # Tiré du nom complet, pas du nom raccourci pour le loader : il donne les
  # noms 8.3 des fichiers .GB/.SAV/.STA du jeu, qui ne doivent pas changer.
  prefix=$(printf '%s' "$long" | tr -c 'a-zA-Z0-9_' '_' | sed -E 's/_+$//')
  [ -n "$prefix" ] || prefix="game"
  case "$prefix" in [0-9]*) prefix="g$prefix" ;; esac
  while in_array "$prefix" ${used_prefixes[@]+"${used_prefixes[@]}"}; do prefix="${prefix}_2"; done

  assess "$dir" "$sha"
  R_path[n]="$rom_path"; R_base[n]="$(basename "$rom_path")"
  R_clean[n]="$dir"; R_prefix[n]="$prefix"; R_sha[n]="$sha"
  R_status[n]="$S_todo"; R_ver[n]="${S_tag}"
  used_prefixes+=("$prefix"); used_dirs+=("$dir"); used_shas+=("$sha")
  n=$((n + 1))
done

if [ "$n" -eq 0 ]; then
  echo "Rien à faire ($n_dup doublon(s) ignoré(s))."
  exit 0
fi

# ---- Choix des jeux --------------------------------------------------------

interactive=0
[ -t 0 ] && [ "$ALL" != 1 ] && interactive=1

declare -a SEL=() KEPT=()
todo_count=0
for ((i = 0; i < n; i++)); do
  [ "${R_status[i]}" != 0 ] && todo_count=$((todo_count + 1))
done

if [ "$interactive" = 1 ]; then
  echo; echo "ROMs de '$SRC_DIR' (gbrecomp v$CUR_VERSION) :"
  for ((i = 0; i < n; i++)); do
    extra=""
    [ "${R_clean[i]}" != "${R_base[i]%.*}" ] && extra="  <== ${R_base[i]}"
    printf " %2d. %-28s [%s]%s\n" $((i + 1)) "${R_clean[i]}" "${R_ver[i]}" "$extra"
  done
  [ "$n_dup" -gt 0 ] && echo "($n_dup doublon(s) ignoré(s))"
  echo
  read -r -p "Jeux à traiter [Entrée = ce qui a à faire ($todo_count), a = tout, ex: 1,3-5, q = rien] : " ans
  case "$ans" in
    q|Q) exit 0 ;;
    "" ) for ((i = 0; i < n; i++)); do [ "${R_status[i]}" != 0 ] && SEL+=("$i"); done ;;
    a|A) for ((i = 0; i < n; i++)); do SEL+=("$i"); done ;;
    *  ) IFS=',' read -ra parts <<< "$ans"
         for part in "${parts[@]}"; do
           part=$(printf '%s' "$part" | tr -d '[:space:]')
           if [[ "$part" == *-* ]]; then
             from=${part%-*}; to=${part#*-}
             for ((j = from; j <= to; j++)); do
               [ "$j" -ge 1 ] && [ "$j" -le "$n" ] && SEL+=("$((j - 1))")
             done
           elif [[ "$part" =~ ^[0-9]+$ ]] && [ "$part" -ge 1 ] && [ "$part" -le "$n" ]; then
             SEL+=("$((part - 1))")
           fi
         done ;;
  esac

  # Proposer la mise à jour des firmwares faits par une autre version.
  upd=0
  for i in ${SEL[@]+"${SEL[@]}"}; do [ "${R_status[i]}" = 2 ] && upd=$((upd + 1)); done
  if [ "$upd" -gt 0 ]; then
    echo; echo "Firmware(s) fait(s) par une autre version (ou de version inconnue) :"
    for i in "${SEL[@]}"; do
      [ "${R_status[i]}" = 2 ] && echo "  - ${R_clean[i]}  [${R_ver[i]}]"
    done
    read -r -p "Reconstruire avec gbrecomp v$CUR_VERSION ? [O/n] " o
    case "$o" in n|N*)
      rest=()
      for i in ${SEL[@]+"${SEL[@]}"}; do
        if [ "${R_status[i]}" = 2 ]; then KEPT+=("$i"); else rest+=("$i"); fi
      done
      SEL=(${rest[@]+"${rest[@]}"})
      ;;
    esac
  fi
else
  # Non interactif : ce qui a à faire, sauf mise à jour de version
  # (UPDATE=1 pour l'autoriser) et déjà-à-jour (FORCE=1 pour refaire).
  for ((i = 0; i < n; i++)); do
    case "${R_status[i]}" in
      1) SEL+=("$i") ;;
      2) if [ "$UPDATE" = 1 ]; then SEL+=("$i"); else KEPT+=("$i"); fi ;;
      0) SEL+=("$i") ;; # déjà à jour : icônes complétées sans recompiler (FORCE=1 refait)
    esac
  done
fi

# ---- Traitement ------------------------------------------------------------

[ "$PLAY" = "1" ] && JOBS=1
STATUS_DIR=$(mktemp -d)
trap 'rm -rf "$STATUS_DIR"' EXIT
running=0
echo
echo "${#SEL[@]} jeu(x) à traiter, ${JOBS} en parallèle, ${AUTO_SEEDS} parties automatiques par ROM (${NPROC} cœurs)"
if [ ${#KEPT[@]} -gt 0 ]; then
  echo "Firmware(s) conservé(s) sans reconstruction :"
  for i in "${KEPT[@]}"; do echo "  - ${R_clean[i]}  [${R_ver[i]}]"; done
  [ "$interactive" = 0 ] && echo "  (UPDATE=1 pour les reconstruire avec v$CUR_VERSION)"
fi

run_one() { # $1 rom, $2 préfixe, $3 dossier du jeu, $4 sha1, $5 index
  local i="$5"
  if build_game "$1" "$2" "$3" "$4"; then
    echo "$3" > "$STATUS_DIR/ok.$i"
  else
    echo "$3" > "$STATUS_DIR/fail.$i"
    return 1
  fi
}

for index in ${SEL[@]+"${SEL[@]}"}; do
  i="$index"
  if [ "${R_status[i]}" = 0 ] && [ "$FORCE" != 1 ]; then
    # Déjà à jour : on ne recompile pas ; icônes manquantes complétées.
    gbc=""
    case "${R_base[i]##*.}" in gbc|GBC) gbc=gbc ;; esac
    if fetch_icons "$DEST_DIR/${R_clean[i]}" "${R_clean[i]}" "${R_base[i]}" "$gbc"; then
      echo "[=] ${R_clean[i]} : déjà à jour (v$CUR_VERSION), ignoré"
    else
      echo "[=] ${R_clean[i]} : déjà à jour (v$CUR_VERSION), ignoré (icône absente)"
    fi
    echo "${R_clean[i]}" > "$STATUS_DIR/skip.$i"
    continue
  fi
  rom_path="${R_path[i]}"; prefix="${R_prefix[i]}"; dir="${R_clean[i]}"

  if [ "$JOBS" -le 1 ]; then
    echo "=========================================="
    echo "[+] $dir  (${R_base[i]})"
    echo "=========================================="
    if run_one "$rom_path" "$prefix" "$dir" "${R_sha[i]}" "$i"; then
      echo "[+] Terminé : $DEST_DIR/$dir/$dir.bin"
    else
      echo "[x] Échec pour ${R_base[i]} (ROM refusée ou firmware trop gros : voir le message ci-dessus)"
    fi
    echo ""
  else
    mkdir -p "$OUT_ROOT/$prefix"
    (
      if run_one "$rom_path" "$prefix" "$dir" "${R_sha[i]}" "$i" > "$OUT_ROOT/$prefix/compile.log" 2>&1; then
        echo "[+] $dir"
      else
        echo "[x] $dir (voir $OUT_ROOT/$prefix/compile.log)"
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

n_ok=0 n_fail=0 n_skip=0
for f in "$STATUS_DIR"/ok.*;  do [ -f "$f" ] && n_ok=$((n_ok + 1)); done
for f in "$STATUS_DIR"/fail.*; do [ -f "$f" ] && n_fail=$((n_fail + 1)); done
for f in "$STATUS_DIR"/skip.*; do [ -f "$f" ] && n_skip=$((n_skip + 1)); done
echo "------------------------------------------"
echo "Réussis : $n_ok   Échecs : $n_fail   Déjà à jour : $n_skip   Conservés : ${#KEPT[@]}"
for f in "$STATUS_DIR"/fail.*; do [ -f "$f" ] && echo "  échec : $(cat "$f")"; done
echo "Jeux dans : $DEST_DIR/<Jeu>/  (à copier tels quels à la racine de la carte SD)"
[ "$n_fail" -eq 0 ]
