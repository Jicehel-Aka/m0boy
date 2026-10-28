#!/bin/bash
# Le lot de jeux profilé : sessions m0search (explorateur + idle +
# enregistrements manuels) par jeu, couverture fusionnée pour Mega Drive et
# Game Boy, fichiers de session bruts pour les coeurs Z80 (sg1000, gg, sms,
# msx) et Vectrex — leurs générateurs exigent l'en-tête magique et les
# lignes « # entry » que la fusion perd — puis régénération pc et aka avec
# --coverage et builds.  PICO-8 reste de côté (le Lua recompilé n'a pas de
# consommateur de couverture).
#
# Flash AKA : le générateur Mega Drive n'a pas de budget et émet tout le
# profil.  Sonic 3 déborde la partition (3,6 Mio) au-delà d'environ
# 14 400 instructions : la profondeur d'idle pilote la couverture (le mode
# démo joue un niveau entier) — 1 000 trames d'idle donnent ~9 100
# instructions, 2 400 en donnent ~12 800 (97,8 % de la partition), 3 000
# débordent.  Ajuster --idle-frames de la session sonic3 si le firmware
# ne passe pas au rapport pio.
#
# Livraison : bash output/jeux/livraison.sh après coup (elle reconstruit
# les .exe mingw et emballe les zips aka).
set -u
cd "$(dirname "$0")/../.." || exit 1
R=build/bin/m0recomp
J=output/jeux
S=tools/profile/m0search.py
export SDL_VIDEODRIVER=dummy
export PATH="${PATH}:/home/m/.local/bin"
FAILURES=""

prof() { # slug system harness sessions frames idle [bios] [--input f ...]
  local slug=$1 sys=$2 harness=$3 sessions=$4 frames=$5 idle=$6 bios=$7
  shift 7
  local out="$J/prof-$slug"
  if [ -f "$out/coverage.merged" ]; then
    echo "=== profil $slug : couverture déjà là ($(wc -l < "$out/coverage.merged") adresses)"
    return 0
  fi
  rm -rf "$out"; mkdir -p "$out"
  echo "=== profil $slug ($sys)"
  local argv=(python3 "$S" run --system "$sys" --harness "$harness" -o "$out"
              --sessions "$sessions" --frames "$frames" --idle-frames "$idle"
              --seed 42 --timeout 600 --interp)
  [ -n "$bios" ] && argv+=(--bios "$bios")
  [ -n "$PROF_ROM" ] && argv+=(--rom "$PROF_ROM")
  argv+=("$@")
  if ! "${argv[@]}" > "$out/run.log" 2>&1; then
    echo "FAILED profil $slug"; tail -5 "$out/run.log"; FAILURES="$FAILURES prof-$slug"; return 1
  fi
  tail -2 "$out/run.log"
}

merged() { # la couverture fusionnée d'un jeu
  local f="$J/prof-$1/coverage.merged"
  [ -f "$f" ] && printf -- '--coverage %q ' "$f"
}
raw() { # les fichiers de session bruts (en-têtes préservés)
  for f in "$J"/prof-$1/*.cov; do printf -- '--coverage %q ' "$f"; done
}

gen() { # slug system rom target covspec
  local slug=$1 sys=$2 rom=$3 target=$4 covspec=$5
  local out="$J/$slug-$target"
  echo "=== gen $slug -> $target (profilé)"
  rm -rf "$out"
  # shellcheck disable=SC2086
  if ! "$R" "roms/$rom" --system "$sys" --target "$target" -o "$out" $($covspec) \
       > "$J/$slug-$target.gen.log" 2>&1; then
    echo "FAILED gen $slug-$target"; tail -3 "$J/$slug-$target.gen.log"; FAILURES="$FAILURES $slug-$target"; return 1
  fi
  grep -E "instructions|regions|compiled:|mode:" "$J/$slug-$target.gen.log" | head -4 | sed 's/^/    /'
}

# ---- phase A : les couvertures -----------------------------------------
# Mega Drive : harnais embarqué, sessions riches ; sonic garde ses six
# enregistrements manuels.
PROF_ROM="" prof sonic    megadrive "$J/sonic-pc/build/md_pc"        6 1500 6000 "" \
  --input logs/md_start_jump.input --input logs/md_start_move.input \
  --input logs/md_start_move_late.input --input logs/sonic_start.input \
  --input logs/sonic_move.input --input output/sonic1.input --manual-frames 2400
PROF_ROM="" prof sonic3   megadrive "$J/sonic3-pc/build/md_pc"       6 1500 6000 ""
PROF_ROM="" prof castle   megadrive "$J/castle-pc/build/md_pc"       6 1500 6000 ""
PROF_ROM="" prof quackshot megadrive "$J/quackshot-pc/build/md_pc"   6 1500 6000 ""
# Game Boy : le harnais _profile du projet, ROM embarquée.
PROF_ROM="" prof sml2     gb "$J/sml2-pc/host/build/Super_Mario_Land_2___6_Golden_Coins__USA__Europe__profile" 4 1500 3600 ""
# Z80 et Vectrex : harnais du dépôt ou harnais embarqué, ROM positionnelle.
PROF_ROM='roms/sg/Chack'"'"'n Pop (Japan).sg'        prof chackn-pop sg1000    build/bin/sg1000_host 4 1200 3600 ""
PROF_ROM='roms/gg/Alien 3 (USA).gg'                  prof alien3     gamegear  build/bin/gg_host     4 1500 3600 ""
PROF_ROM='roms/sms/Ace of Aces (E) [!].sms'          prof ace-of-aces gamegear build/bin/gg_host     4 1500 3600 ""
PROF_ROM='roms/msx/Aleste (1988)(Compile)(jp).rom'   prof aleste     msx       build/bin/msx_host    4 1200 3600 ""
PROF_ROM="" prof raiden    pce       "$J/raiden-pc/host/build/Raiden__USA__host" 4 1500 3600 ""
PROF_ROM="" prof smb       nes       "$J/smb-pc/host/build/Super_Mario_Bros___World__host" 4 1500 3600 ""
PROF_ROM="" prof mine-storm vectrex  "$J/mine-storm-pc/build/vectrex_pc" 4 900 1800 'roms/vectrex/Vectrex BIOS (1982).vec'

# ---- phase B : régénération avec la couverture --------------------------
# Mega Drive et Game Boy : fusion.  Z80 et Vectrex : sessions brutes.
gen sonic     megadrive 'md/Sonic The Hedgehog (USA, Europe).md' pc  "$(merged sonic)"
gen sonic     megadrive 'md/Sonic The Hedgehog (USA, Europe).md' aka "$(merged sonic)"
gen sonic3    megadrive 'md/Sonic The Hedgehog 3 (USA).md'       pc  "$(merged sonic3)"
gen sonic3    megadrive 'md/Sonic The Hedgehog 3 (USA).md'       aka "$(merged sonic3)"
gen castle    megadrive 'md/Castle of Illusion Starring Mickey Mouse (USA, Europe).md' pc  "$(merged castle)"
gen castle    megadrive 'md/Castle of Illusion Starring Mickey Mouse (USA, Europe).md' aka "$(merged castle)"
gen quackshot megadrive 'md/QuackShot Starring Donald Duck ~ QuackShot - I Love Donald Duck - Guruzia Ou no Hihou (World).md' pc "$(merged quackshot)"
gen quackshot megadrive 'md/QuackShot Starring Donald Duck ~ QuackShot - I Love Donald Duck - Guruzia Ou no Hihou (World).md' aka "$(merged quackshot)"
gen sml2      gameboy   'gb/Super Mario Land 2 - 6 Golden Coins (USA, Europe).gb' pc  "$(merged sml2)"
gen sml2      gameboy   'gb/Super Mario Land 2 - 6 Golden Coins (USA, Europe).gb' aka "$(merged sml2)"
gen chackn-pop sg1000   'sg/Chack'"'"'n Pop (Japan).sg' pc  "$(raw chackn-pop)"
gen chackn-pop sg1000   'sg/Chack'"'"'n Pop (Japan).sg' aka "$(raw chackn-pop)"
gen alien3    gg        'gg/Alien 3 (USA).gg'  pc  "$(raw alien3)"
gen alien3    gg        'gg/Alien 3 (USA).gg'  aka "$(raw alien3)"
gen ace-of-aces sms     'sms/Ace of Aces (E) [!].sms' pc  "$(raw ace-of-aces)"
gen ace-of-aces sms     'sms/Ace of Aces (E) [!].sms' aka "$(raw ace-of-aces)"
gen aleste    msx       'msx/Aleste (1988)(Compile)(jp).rom' pc "$(raw aleste)"
gen raiden    pce       'pce/Raiden (USA).pce' pc "$(merged raiden)"
gen raiden    pce       'pce/Raiden (USA).pce' aka "$(merged raiden)"
gen smb       nes       'nes/Super Mario Bros. (World).nes'  pc "$(merged smb)"
gen mine-storm vectrex  'vectrex/3-D Mine Storm (1983).vec'  pc "$(raw mine-storm)"

# ---- phase C : builds ----------------------------------------------------
echo "=== builds pc"
bash "$J/build_pc.sh" || FAILURES="$FAILURES build-pc"
echo "=== builds aka"
bash "$J/build_aka.sh" || FAILURES="$FAILURES build-aka"

echo "=== TERMINÉ${FAILURES:+, échecs :$FAILURES}"
