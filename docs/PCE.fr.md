# PC Engine (`--system pce`)

`--system pce` (extension `.pce`) génère deux formes de projets : le
harness hôte (`--target desktop`, images PPM et WAV hors écran) et la
**fenêtre PC** (`--target pc`) — un shell SDL2 (`targets/pce/platform/
pce_pc.c`) : fenêtre ×3 redimensionnable, clavier (flèches, X = I,
Z = II, Entrée = RUN, Maj = SELECT, Échap quitte), manette SDL2 (croix ou
stick gauche, B = I, A = II, Start = RUN, Back = SELECT) ou joystick générique,
le son du PSG en file
SDL, `--frames`/`--ppm` pour une passe hors écran. La HuCard est
embarquée dans l'exécutable. `tools/pce/build_pc_binaries.sh` construit
le binaire Linux et un **.exe Windows** (MinGW croisé, `SDL2.dll` à
côté), sur le modèle du Mega Drive. Le shell et le harness impriment le
même `run_hash` (la recette du fingerprint est la même), et le .exe
produit l'image du binaire Linux **octet pour octet** — le cœur est
déterministe d'une plateforme à l'autre. C'est la même trajectoire que
la NES : le cœur interprété d'abord, la recompilation HuC6280 → C
viendra ensuite sur le même modèle (`emitter.cpp` + `nes_gen.h` →
`pce_gen.h`).

Le frontend SDL commun à toutes les cibles `--target pc` montre l'image sans
filtre au démarrage. F8 parcourt pixel, DMG, LCD RGB, scanlines et grille ;
F10 change l'échelle et F11 bascule en plein écran. Les manettes sont
détectées au démarrage et lors de leur branchement. Les captures `--ppm` et
`run_hash` restent calculés sur l'image du cœur, indépendamment du filtre.

```bash
tools/pce/build_pc_binaries.sh                       # raiden-linux + raiden.exe + SDL2.dll
build/bin/gbrecomp --system pce "Raiden (USA).pce" --target pc -o out/raiden_pc
cmake -G Ninja -S out/raiden_pc -B out/raiden_pc/build && ninja -C out/raiden_pc/build
out/raiden_pc/build/Raiden__USA__pc --frames 300 --ppm shot.ppm
# frames=300 pc=4e2f run_hash=c6637372  (le harness donne le même)
```

Le harness imprime le `run_hash` (CPU, WRAM, VRAM, SATB, palette, frame),
deterministe — la même loi que les autres runtimes : un projet précis
devra l'imprimer aussi, et le e2e (`tests/test_pce_e2e.py`) tient un
cartouche synthétique dont il vérifie hash et pixels.

## La machine

- **HuC6280** (`huc6280.c`) : le jeu d'instructions complet, comptes de
  cycles et états de reset tirés du cœur mednafen (beetle-pce-libretro,
  `huc6280_ops.inc`), recoupés sur le stub de boot de Raiden lui-même.
  Les opcodes 65C02 (BRA, STZ, TSB/TRB, (zp), BIT #, PHX/PHY…), les siens
  (TAM/TMA en masque de bits, BSR 8 bits — il pousse l'adresse de l'octet
  de displacement, le RTS +1 retombe après l'instruction : une erreur
  d'un octet ici faisait sauter l'instruction suivant chaque BSR et
  gelait le démarrage de Raiden —, ST0/1/2, RMB/SMB, BBR/BBS, TST, les
  cinq block moves TII/TDD/TIN/TIA/TAI à $73/$C3/$D3/$E3/$F3, SET,
  CLA/CLX/CLY, SXY/SAX/SAY, CSL/CSH), le mode T (ADC/AND/ORA/EOR sur
  zp[X]), le décimal, les vecteurs ($FFF6 BRK/IRQ2 — le BRK du 6280
  vecte bien par $FFF6, vérifié contre mednafen —, $FFF8 IRQ1=VDC,
  $FFFA timer). Les opcodes illégaux sont des NOP d'un octet et deux
  cycles — aucun jeu du périmètre n'en dépend. CSH/CSL posent le drapeau
  de vitesse sans changer le décompte (le premier jeu du périmètre passe
  en CSH tôt).
- **Mémoire** : les huit registres MPR fenêtrent l'espace 2 Mio par 8 Kio ;
  TAM bit 0 = MPR0. La WRAM de 8 Kio est le page $F8 **et ses miroirs
  $F9-$FB** (le hardware les décode sur quatre pages — Raiden mappe $FB).
  La page $FF est la page matérielle ($0000-$1FFF au boot, MPR0=$FF) ;
  le reste est bus ouvert ($FF). Au reset : MPR7=$00 (les vecteurs dans la
  page 0 de la cartouche — vérifié sur Raiden : RESET=$E000 y pointe), P=I,
  tous les IRQ sources armées.
- **VDC (HuC6270)** (`vdc.c`) : 64 Kio de VRAM, les 18 registres, le port
  VRAM (MAWR/MARR avec auto-incrément de CR bits 11-12), l'écriture des
  registres **par octet** (le LSB s'applique à l'octet bas, le MSB à
  l'octet haut — le port VRAM attend lui le MSB, comme le cœur mednafen),
  le DMA VRAM→VRAM complet à l'écriture de LENR (SOUR/DESR avec les sens
  de DCR), la SATB DMA (DVSSR ou DCR bit 4, copiée à la première ligne
  après la zone active), le BAT (largeur 32/64/128, hauteur 32/64 par
  MWR), les caractères 4bpp, 16 sprites de ligne (overflow → IRQ si CR
  bit 1), la priorité sprite/fond et le bit de priorité, la collision du
  sprite 0. La machine verticale suit les phases VSW/VDS/VDW/VCR du cœur
  mednafen, un pas par ligne : latches BYR/BXR, RCRCount (comparé à
  RCR-0x40 pour l'IRQ raster), IRQ VBlank et SATB à la première ligne
  après VDW.
- **VCE** : la table de 512 couleurs (B bits 0-2, R 3-5, G 6-8) et sa
  LUT RGB565 ; la couleur 0 de chaque palette montre l'entrée 0 (le fond),
  comme le FixPCache de mednafen.
- **Timer** : un tick tous les 1024 cycles CPU, IRQ TIQ calée jusqu'à
  l'acquittement ($1402 en lecture ou $1403 en écriture), le rechargement
  au tick suivant. Les ticks tombent aux frontières de ligne (le jitter
  reste sous une ligne).
- **Pad** ($1000) : SEL choisit le nibble (0 : I/II/SELECT/RUN, 1 :
  HAUT/DROITE/BAS/GAUCHE — l'ordre des bits de mednafen, recoupé sur la
  routine de Raiden : son test `CMP #$0C` = SELECT+RUN dans le nibble
  SEL=0), bouton pressé = 0, bits 4-5 toujours à 1, bit 6 à 0 (une PC
  Engine — la sonde CD du menu HuCard de Raiden en dépend), bit 7 à 1
  (pas de CD).
- **PSG** (`psg.c`) : six canaux wavetable 32 échantillons, fréquence
  12 bits (période = 2×F horloge maître), volume 5 bits, mode DDA (le
  dernier échantillon écrit), bruit en LFSR 18 bits (le polynôme du chip
  n'est pas publié : l'approximation est assumée). Ni LFO ni balance.

## Ce qui n'est pas modélisé

Les états d'attente CPU sur accès VDC/VCE, le vol de cycles du DMA (un
DMA finit d'un coup, déterministe), le bit BSY du statut, la fenêtre de
fetch CG des sprites, le mode CG « one word », l'horloge point de la VCE
(la largeur vient de HDR), la BSRAM et le CD-ROM², le multitap. Un jeu qui
sonde BSY pendant un DMA verra le DMA déjà fini.

## Le harness

`targets/pce/host/pce_host.c`, le contrat des autres runtimes :
`--frames`, `--ppm` (la fenêtre active : largeur = (HDW+1)×8, hauteur =
VDW+1), `--wav`, `--input` (script « frame boutons », noms `i ii select
run up down left right`), `run_hash=` sur la sortie. La ligne de lecture
du pad et la sonde `m0input` suivent le modèle commun (une session gm0
rejouera sur le harness quand l'application gm0 saura la lancer).

## Ce qui a été vérifié

- **Raiden (USA)**, 768 Kio : boot complet (le stub du menu HuCard, la
  sonde CD, l'init des MPR, l'effacement RAM, la programmation VDC depuis
  la table du stub, la routine de palette **générée en WRAM** par le jeu
  — un TIA autogénéré vers le port VCE), IRQ VBlank + RCR, SATB DMA,
  écran titre exact : kanji 雷電, logo RAIDEN™, texte « FROM NEC / © 1990
  SEIBU KAIHATSU INC. / ALL RIGHTS RESERVED / © 1991 HUDSON SOFT ».
  RUN sur le titre lance la partie et la partie se joue : le vaisseau
  décolle du porte-avions, la stage 1 défile (océan, côte, forêt, chars),
  les escadrilles attaquent avec leurs tirs visibles, les entrées
  répondent (déplacements, tir). Le gel « avant de vraiment jouer »
  d'avant le correctif BSR est diagnostiqué et compris : la boucle objet
  du jeu (banque 8, $4900-$4912) sautait le PLA qui suit son `BSR`, un
  octet mort restait dans la pile, le RTS final partait dans la page
  matérielle et la tempête de BRK→RTI qui suit laissait le programme
  principal attendre le drapeau vblank $207E pour toujours.
  run_hash stable d'une exécution à l'autre (c6637372 à l'image 300,
  harness desktop = binaire PC).
- **Le budget CPU par ligne suit CSL/CSH** : Raiden démarre en CSL
  (1,79 MHz) et passe tôt en CSH (7,16 MHz) ; une ligne VDC fait 1365
  horloges maîtresses, soit 455 cycles CPU en CSH mais ~114 en CSL. Le
  cœur donnait 455 cycles quoi qu'il arrive : tout le boot CSL tournait
  4× trop vite, l'état calibré au démarrage divergeait, et RUN lançait
  une partie restée en mode scripté — plus aucun tir, aucun ennemi,
  aucune entrée prise en compte. Le budget compte maintenant des
  horloges maîtresses (1365 par ligne, divisées par 3 en CSH, 12 en
  CSL), comme le cœur mednafen ; l'attract suit de nouveau la vraie
  timeline (écran PUSH RUN vers l'image 300 des deux côtés) et RUN
  démarre une vraie partie.
- **Comparaison de référence** : le cœur mednafen (beetle-pce-libretro)
  piloté par un mini frontend libretro (avec `retro_set_controller_port_device`
  — sans lui le pad de la référence reste mort, et la démo auto-jouée
  masquait tout écart d'entrée). Le titre colle image pour image, RUN y
  démarre une vraie partie des deux côtés, et les deux parties réelles
  montrent le même déroulé (lancement porte-avions, océan, côte, combat,
  score). Restent des écarts fins non tranchés : la démo de l'attract,
  le rythme exact du niveau (le cœur m0boy atteint le mini-boss sphère
  plus tôt que la référence, qui meurt avant sur la même entrée) et le
  décalage de scroll de quelques pixels sur le titre.
- **e2e synthétique** (`tests/test_pce_e2e.py`) : cartouche générée en
  Python (vecteurs, TAM, VDC, VCE), deux exécutions même hash, pixels du
  fond et du caractère vérifiés.

## Chantiers suivants

- L'attract : la démo jouée par la machine (mednafen la joue entre son
  cycle logo → mur → démo → PUSH RUN) reste sautée — le cœur m0boy passe
  par un écran de charge noir vers l'image 700 puis rebondit au titre.
  L'init s'enlise dans le chargeur VRAM par ligne (banque 5, $4411-$4500,
  ~4,5 unités par frame côté m0boy). Trancher ce qui manque (un drapeau,
  un timing de charge) contre la référence.
- Le rythme du niveau : sur la même entrée alignée sur RUN, le cœur
  m0boy atteint la sphère (mini-boss) vers RUN+1200 quand la référence
  en est encore à la première section plage (elle y meurt vers RUN+2600
  et propose le continue « CREDIT 6 »). Recouper frame à frame les deux
  parties réelles pour trancher ce qui reste (calibration d'un temporisateur,
  diff de trajectoire d'état WRAM au boot).
- La couche sprite : les gros glyphs du titre tombent juste, le texte
  sprite des bords de lignes montre des artefacts de découpe (limite des
  16 sprites de ligne ou fetch des demi-motifs 32 px — à trancher contre
  la même référence).
- Le recompilateur HuC6280 → C (le modèle NES : régions, profils depuis
  le harness, interpréteur en repli) et la cible META — la VRAM de 64 Kio
  ne tient pas dans les 32 Kio de SRAM : il faudra la fenêtrer en flash
  comme la ROM Sega.
- Le PSG complet (LFO, balance, polynôme de bruit) et l'entrée audio du
  harness comparée à la référence.
