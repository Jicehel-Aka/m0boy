#!/usr/bin/env bash
# Lanceur du profil d'une ROM, appelé par le play.sh que compile.sh écrit
# dans profiles/<source>/<Jeu>/ :
#
#   ./play.sh [meta|pokitto|espboy] [options de gm0]
#       Une session de jeu dans gm0, avec le firmware construit pour cette
#       console (le dernier zip produit par compile.sh ; sans console : la
#       première construite, meta, pokitto puis espboy).  Elle est
#       enregistrée pour l'entraînement :
#         - la carte est neuve à chaque session (sauvegarde vierge, pas
#           d'états), pour que la partie se rejoue depuis l'allumage ;
#         - gm0 note les boutons que reçoit le jeu, image par image
#           (gm0/<console>-<date>.keys, gm0 --record-input) ;
#         - ils deviennent coverage-gm0-<date>.input, rejoué aussitôt dans le
#           harnais PC : code exécuté (.cov) et lectures de données ; les
#           lancements suivants de compile.sh compilent ce code et rejouent
#           la session quand le profil est refait ;
#         - cycles par fonction du firmware (EMU_PROF) dans
#           gm0/<console>-<date>.cyc, résumés à la sortie.
#       F2 prend un instantané, F4 y revient : gm0 redémarre et rejoue en
#       avance rapide les touches jusque-là, l'enregistrement reste rejouable
#       depuis l'allumage.  Redémarrer (F5) ou recharger un état du menu du
#       firmware coupe en revanche la partie rejouable à cet endroit.
#       GM0=/chemin/vers/gm0 (défaut : ../gm0/build/bin/gm0 à côté du dépôt).
#       Les cycles de gm0 sont des estimations, pas des mesures sur console.
#   ./play.sh reprendre [<date>] [console]
#       Reprend une session enregistrée (la dernière par défaut) à son
#       dernier instantané (F2), ou là où elle s'est arrêtée : avance rapide
#       jusque-là, puis la main au joueur ; la nouvelle session contient tout
#       depuis l'allumage (celle reprise depuis sa fin est remplacée).
#   ./play.sh --carte [console] [options de gm0]
#       Jeu libre sur une carte gardée d'une partie à l'autre (sauvegardes,
#       états : gm0/<console>/card) ; profil de cycles, pas d'enregistrement.
#   ./play.sh rejouer [<date>] [console]
#       Rejoue dans gm0 une session enregistrée (la dernière par défaut),
#       touche pour touche (gm0 --play-input), sur une carte neuve.
#   ./play.sh record
#       (Game Boy) une partie à la main dans la fenêtre du harnais hôte,
#       enregistrée comme les autres (coverage-<date>.cov, .input,
#       romdata-<date>.prof).
#   ./play.sh report
#       Refait profile.json (parties, carte de la ROM, builds, runs gm0).
set -uo pipefail

# Tout dans main : bash lit le script au fil de l'exécution, une fonction
# est lue en entier d'abord (modifier ce fichier ne dérange pas une session
# en cours).
main() {
store="${1:?dossier du profil}"; shift
here="$(cd -P "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
report() { python3 "$here/m0profile.py" report "$store" "$@"; }
env_get() { # $1 fichier key=valeur, $2 clé
  python3 - "$1" "$2" <<'PY'
import shlex, sys
for line in open(sys.argv[1], encoding='utf-8'):
    key, sep, value = line.rstrip('\n').partition('=')
    if sep and key == sys.argv[2]:
        print(''.join(shlex.split(value)) if value else '')
PY
}
build_get() { sed -n "/^---$/q; s/^$2=//p" "$1"; } # en-tête de build-<console>.txt

[ -f "$store/rom.env" ] || { echo "[x] $store/rom.env absent : passer d'abord la ROM dans compile.sh."; exit 1; }
NAME=$(env_get "$store/rom.env" NAME)
SYSTEM=$(env_get "$store/rom.env" SYSTEM)
ROM=$(env_get "$store/rom.env" ROM)
HOST=$(env_get "$store/rom.env" HOST)

# Une partie enregistrée (coverage-<id>.input) rejouée dans le harnais PC :
# code exécuté, et lectures de données en Game Boy.
replay_session() { # $1 id
  local inp="$store/coverage-$1.input" last
  [ -n "$HOST" ] && [ -x "$HOST" ] || { echo "[!] Harnais PC absent ($HOST) : compile.sh rejouera la partie."; return 1; }
  if [ "$SYSTEM" = gameboy ]; then
    last=$(awk 'END { print $1 + 120 }' "$inp")
    "$HOST" --interp --frames "$last" --input "$inp" --coverage "$store/coverage-$1.tmp" \
      --rom-profile-out "$store/romdata-$1.prof" > /dev/null || return 1
  else
    last=$(awk 'END { print $1 + 60 }' "$inp")
    "$HOST" "$ROM" --frames "$last" --input "$inp" --coverage "$store/coverage-$1.tmp" > /dev/null || return 1
  fi
  mv "$store/coverage-$1.tmp" "$store/coverage-$1.cov"
}

mode="${1:-}"
case "$mode" in
  -h|--help|help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
  report) report; exit $? ;;
  record)
    play=$(env_get "$store/rom.env" HOST_PLAY)
    if [ "$SYSTEM" != gameboy ]; then
      echo "[x] record : seul le harnais Game Boy a une fenêtre qui enregistre ; en $SYSTEM, ./play.sh (gm0) enregistre la session."
      exit 1
    fi
    [ -n "$play" ] && [ -x "$play" ] || { echo "[x] Harnais de jeu absent ($play) : relancer compile.sh pour ce jeu."; exit 1; }
    stamp=$(date +%Y%m%d-%H%M%S)
    echo "[!] $NAME : partie enregistrée. Flèches | X=A | Z=B | Entrée=Start | Retour=Select | Échap=Quitter"
    "$play" --interp --coverage "$store/coverage-$stamp.cov" --rom-profile-out "$store/romdata-$stamp.prof" \
      --record-input "$store/coverage-$stamp.input" || true
    if [ -s "$store/coverage-$stamp.cov" ]; then
      echo "[i] Partie $stamp ajoutée au profil ; ./compile.sh reconstruira les firmwares de ce jeu."
    else
      rm -f "$store/coverage-$stamp".* "$store/romdata-$stamp.prof"
      echo "[!] Rien d'enregistré."
    fi
    report; exit 0 ;;
esac

persistent=0 replay="" resume="" resume_at=""
if [ "$mode" = --carte ]; then persistent=1; shift
elif [ "$mode" = reprendre ]; then
  shift
  case "${1:-}" in [0-9]*) resume="$1"; shift ;; esac
  if [ -z "$resume" ]; then
    resume=$(ls "$store"/gm0/*-*.keys 2>/dev/null | sed 's/.*-\([0-9]\{8\}-[0-9]\{6\}\)\.keys$/\1/' | sort | tail -n1)
  fi
  [ -n "$resume" ] || { echo "[x] Aucune session gm0 enregistrée."; exit 1; }
  resume_keys=$(ls "$store"/gm0/*-"$resume".keys | head -n1)
  # le dernier instantané, sinon la fin de la session
  resume_at=$(awk '$1 == "s" { s = $2 } $1 == "e" { e = $2 } END { print s != "" ? s : e }' "$resume_keys")
  [ -n "$resume_at" ] && [ "$resume_at" -gt 0 ] || { echo "[x] Session $resume sans instantané ni fin lisible."; exit 1; }
  replay="" # la console de la session, comme pour rejouer
  case "${1:-}" in meta|pokitto|espboy) ;; *) set -- "$(basename "$resume_keys" | sed 's/-.*//')" "$@" ;; esac
  [ "${1:-}" = espboy ] && { echo "[x] Instantanés : pas sur l'ESPboy."; exit 1; }
elif [ "$mode" = rejouer ]; then
  shift
  case "${1:-}" in [0-9]*) replay="$1"; shift ;; esac
  if [ -z "$replay" ]; then
    replay=$(ls "$store"/gm0/*-*.keys 2>/dev/null | sed 's/.*-\([0-9]\{8\}-[0-9]\{6\}\)\.keys$/\1/' | sort | tail -n1)
  fi
  [ -n "$replay" ] || { echo "[x] Aucune session gm0 enregistrée."; exit 1; }
fi
target=""
case "${1:-}" in meta|pokitto|espboy) target="$1"; shift ;; esac
if [ -n "$replay" ] && [ -z "$target" ]; then # la console de la session
  target=$(ls "$store"/gm0/*-"$replay".keys 2>/dev/null | head -n1 | sed 's#.*/\([a-z]*\)-[0-9-]*\.keys$#\1#')
fi
if [ -z "$target" ]; then
  for t in meta pokitto espboy; do [ -f "$store/build-$t.txt" ] && { target=$t; break; }; done
  [ -n "$target" ] || { echo "[x] Aucun firmware construit pour $NAME : lancer compile.sh."; exit 1; }
fi

build="$store/build-$target.txt"
[ -f "$build" ] || { echo "[x] Pas de firmware $target pour $NAME : TARGET=$target ./compile.sh"; exit 1; }
zip=$(build_get "$build" zip)
[ -f "$zip" ] || { echo "[x] $zip absent : relancer TARGET=$target ./compile.sh"; exit 1; }
gm0="${GM0:-$repo/../gm0/build/bin/gm0}"
[ -x "$gm0" ] || { echo "[x] gm0 absent ($gm0) : construire gm0 (github.com/r043v/gm0) ou définir GM0."; exit 1; }

extract() { # $1 zip, $2 carte, $3 : 1 = garder sauvegardes et états déjà là
  python3 - "$1" "$2" "$3" <<'PY' || exit 1
import os, sys, zipfile
keep = ('.sav', '.sta', '.st8')
with zipfile.ZipFile(sys.argv[1]) as z:
    for info in z.infolist():
        dest = os.path.join(sys.argv[2], info.filename)
        if info.is_dir() or (sys.argv[3] == '1' and dest.lower().endswith(keep) and os.path.exists(dest)):
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with z.open(info) as src, open(dest + '.tmp', 'wb') as out:
            out.write(src.read())
        os.replace(dest + '.tmp', dest)
PY
}
work="$store/gm0/$target"
if [ "$persistent" = 1 ]; then
  # Carte gardée ; un nouveau zip remplace le firmware et les fichiers du
  # jeu, sans toucher aux sauvegardes ni aux états.
  card="$work/card"
  mkdir -p "$card" || exit 1
  stamp_zip="$(stat -c '%Y %s' "$zip") $zip"
  if [ "$(cat "$work/.zip-stamp" 2>/dev/null)" != "$stamp_zip" ]; then
    extract "$zip" "$card" 1
    printf '%s\n' "$stamp_zip" > "$work/.zip-stamp"
  fi
else
  # Carte neuve : sans fichier d'états le menu n'offre pas de les charger ;
  # en Sega la RAM de cartouche part à zéro, comme dans le harnais PC.
  card="$work/session"
  rm -rf "$card" && mkdir -p "$card" || exit 1
  extract "$zip" "$card" 0
  find "$card" -type f \( -iname '*.sta' -o -iname '*.st8' \) -delete
  if [ "$SYSTEM" != gameboy ]; then
    find "$card" -type f -iname '*.sav' -print0 | while IFS= read -r -d '' f; do
      head -c "$(stat -c %s "$f")" /dev/zero > "$f"
    done
  fi
fi
game=$(basename "$zip" .zip)
args=()
case "$target" in
  meta)    fw=$(ls "$card/$game"/*.BIN 2>/dev/null | head -n1); args=("$fw" "$card") ;;
  pokitto) fw="$card/$game/$game.bin"; args=("$fw" "$card" --target pokitto) ;;
  espboy)  fw="$zip"; args=("$zip") ;; # gm0 lit le zip ESPboy tel quel
esac
[ -f "$fw" ] || { echo "[x] Firmware introuvable dans $zip"; exit 1; }

stamp=$(date +%Y%m%d-%H%M%S)
cyc="$store/gm0/$target-$stamp.cyc"
keys="$store/gm0/$target-$stamp.keys"
ram="$store/gm0/$target-$stamp.ram" # compteurs du firmware : calibration du plan de stockage
[ "$target" = espboy ] || args+=(--dump-ram "$ram")
if [ -n "$replay" ]; then
  src=$(ls "$store"/gm0/*-"$replay".keys | head -n1)
  echo "[i] $NAME ($target) : session $replay rejouée dans gm0"
  args+=(--play-input "$src")
elif [ -n "$resume" ]; then
  echo "[i] $NAME ($target) : session $resume reprise à l'image gm0 $resume_at (avance rapide), enregistrée ($keys)"
  args+=(--play-input "$resume_keys" --resume-at "$resume_at" --record-input "$keys")
elif [ "$persistent" = 0 ]; then
  echo "[i] $NAME ($target) dans gm0, session enregistrée ($keys)"
  args+=(--record-input "$keys")
else
  echo "[i] $NAME ($target) dans gm0, carte persistante"
fi
EMU_PROF="$cyc" "$gm0" "${args[@]}" "$@"; rc=$?
if [ -s "$cyc" ]; then
  python3 "$here/m0profile.py" top "$store" "$cyc" 15
  if [ -s "$ram" ] && { [ "$SYSTEM" = gg ] || [ "$SYSTEM" = sms ]; }; then
    python3 "$here/m0profile.py" calibrate "$store" "$cyc" "$ram" || true
  fi
else
  rm -f "$cyc"
  [ "$target" = espboy ] && echo "[i] (pas de profil de cycles EMU_PROF pour l'ESPboy)"
fi
if [ -f "$keys" ]; then
  id="gm0-$stamp"
  frames=$(python3 "$here/m0profile.py" session "$store" "$keys" "$id")
  if [ "${frames:-0}" -gt 0 ] 2>/dev/null; then
    head -n1 "$store/coverage-$id.input" | grep -q 'cut:' && echo "[!] $(head -n1 "$store/coverage-$id.input" | sed 's/.*cut: /Partie coupée : /')"
    echo "[i] Session $id : $frames images du jeu ; rejouée dans le harnais PC..."
    replay_session "$id" && echo "[i] Code de la session ajouté au profil ; ./compile.sh reconstruira les firmwares de ce jeu."
    # reprise depuis la fin de l'ancienne session : la nouvelle la contient
    if [ -n "$resume" ] && [ "$resume_at" = "$(awk '$1 == "e" { e = $2 } END { print e }' "$resume_keys")" ]; then
      rm -f "$store/coverage-gm0-$resume".* "$resume_keys"
      echo "[i] Session $resume remplacée par $id (qui la contient depuis l'allumage)"
    fi
  else
    rm -f "$keys"
    echo "[!] Aucun bouton du jeu enregistré (firmware sans descripteur m0input : refaire le build avec compile.sh)."
  fi
fi
report --quiet
exit $rc
}
main "$@"
