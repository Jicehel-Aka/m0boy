#!/bin/bash
# Vérification du lot profilé : pour chaque jeu pc, une sonde en natif et la
# même en interpréteur doivent donner le même run_hash (Mega Drive exact ;
# les autres coeurs non précis plient les cycles par région — l'écart visible
# n'est qu'un décalage de sous-trame, vérifier alors les captures --ppm, qui
# doivent être identiques ou décalées d'une phase).  SDL fantôme partout ;
# les comptes aka viennent des logs pio.
#
# Pièges connus :
#  - les jeux à SRAM (sonic3) écrivent <cartouche>.sav dans le répertoire
#    courant : passer --save jetable aux deux sondes, sinon la seconde part
#    de la SRAM écrite par la première et les run_hash divergent à tort ;
#  - le harnais PCE n'écrit --ppm que si une trame active a été rendue.
set -u
cd "$(dirname "$0")/../.." || exit 1
J=output/jeux
export SDL_VIDEODRIVER=dummy

probe() { # label natif-cmd -- interp-cmd
  local label=$1 nat_out int_out nh ih
  shift
  local -a nat=() int=(); local side=nat
  for a in "$@"; do
    if [ "$a" = "--" ]; then side=int; continue; fi
    if [ "$side" = nat ]; then nat+=("$a"); else int+=("$a"); fi
  done
  nat_out=$(timeout 300 "${nat[@]}" 2>&1 | tail -3 | tr '\n' ' ')
  int_out=$(timeout 300 "${int[@]}" 2>&1 | tail -3 | tr '\n' ' ')
  nh=$(echo "$nat_out" | grep -o 'run_hash[= ]*[0-9a-f]*' | head -1 | grep -o '[0-9a-f]*$')
  ih=$(echo "$int_out" | grep -o 'run_hash[= ]*[0-9a-f]*' | head -1 | grep -o '[0-9a-f]*$')
  local verdict="HASH DIFFERENT"
  [ -n "$nh" ] && [ "$nh" = "$ih" ] && verdict="hash ok"
  printf '%-12s %s\n             natif  : %s\n             interp : %s\n' \
    "$label" "$verdict" "$(echo $nat_out | cut -c1-160)" "$(echo $int_out | cut -c1-160)"
}

for g in sonic sonic3 castle quackshot; do
  b=$J/$g-pc/build/md_pc
  probe "$g" "$b" --frames 1200 --save /tmp/v-$g-n.sav -- \
            "$b" --interp --frames 1200 --save /tmp/v-$g-i.sav
done

P=$J/sml2-pc/host/build/Super_Mario_Land_2___6_Golden_Coins__USA__Europe_
probe sml2 "${P}_host" --frames 3600 -- "${P}_profile" --interp --frames 3600

for g in chackn-pop alien3 ace-of-aces aleste; do
  b=$(ls $J/$g-pc/host/build/*_host 2>/dev/null | head -1)
  [ -z "$b" ] && { echo "$g : harnais absent"; continue; }
  probe "$g" "$b" --frames 1200 -- "$b" --interp --frames 1200
done

b=$J/raiden-pc/host/build/Raiden__USA__host
probe raiden "$b" --frames 1200 -- "$b" --interp --frames 1200

b=$J/smb-pc/host/build/Super_Mario_Bros___World__host
probe smb "$b" --frames 1200 -- "$b" --interp --frames 1200

b=$J/mine-storm-pc/build/vectrex_pc
probe mine-storm "$b" --bios 'roms/vectrex/Vectrex BIOS (1982).vec' --frames 1800 -- \
                  "$b" --bios 'roms/vectrex/Vectrex BIOS (1982).vec' --interp --frames 1800

echo "=== tailles aka (rapports pio)"
for d in sonic sonic3 castle quackshot sml2 chackn-pop alien3 ace-of-aces raiden; do
  s=$(grep -hE "RAM:|Flash:" "$J/$d-aka.pio.log" 2>/dev/null | tail -2 | tr '\n' ' ')
  printf '%-12s %s\n' "$d" "${s:-absent}"
done
