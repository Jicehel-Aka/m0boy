# PC Engine (`--system pce`)

`--system pce` (extension `.pce`) génère un projet PC Engine : la HuCard
est interprétée par le runtime HuC6280/VDC (`targets/pce/runtime/`) sur le
harness hôte (`--target desktop`). C'est la même trajectoire que la NES :
le cœur interprété d'abord, la recompilation HuC6280 → C viendra ensuite
sur le même modèle (`emitter.cpp` + `nes_gen.h` → `pce_gen.h`).

```bash
build/bin/gbrecomp --system pce "Raiden (USA).pce" --target desktop -o out/raiden_pce
cmake -G Ninja -S out/raiden_pce/host -B out/raiden_pce/host/build
ninja -C out/raiden_pce/host/build
out/raiden_pce/host/build/Raiden__USA__host --frames 300 --ppm shot.ppm
# frames=300 cycles=35890450 pc=4e2f run_hash=97ae6d24
```

Le harness imprime le `run_hash` (CPU, WRAM, VRAM, SATB, palette, frame),
deterministe — la même loi que les autres runtimes : un projet précis
devra l'imprimer aussi, et le e2e (`tests/test_pce_e2e.py`) tient un
cartouche synthétique dont il vérifie hash et pixels.

## La machine

- **HuC6280** (`huc6280.c`) : le jeu d'instructions complet, comptes de
  cycles et états de reset tirés du cœur mednafen (beetle-pce-libretro,
  `huc6280_ops.inc`), recoupés sur le stub de boot de Raiden lui-même.
  Les opcodes 65C02 (BRA, STZ, TSB/TRB, (zp), PHX/PHY…), les siens (TAM/
  TMA, BSR 8 bits, ST0/1/2, RMB/SMB, BBR/BBS, TST, les cinq block moves
  TII/TDD/TIN/TIA/TAI à $73/$C3/$D3/$E3/$F3, SET, CLA/CLX/CLY, SXY/SAX/
  SAY, CSL/CSH), le mode T (ADC/AND/ORA/EOR sur zp[X]), le décimal, les
  vecteurs ($FFF6 BRK/IRQ2, $FFF8 IRQ1=VDC, $FFFA timer). Les opcodes
  illégaux sont des NOP d'un octet et deux cycles — aucun jeu du périmètre
  n'en dépend. CSH/CSL posent le drapeau de vitesse sans changer le
  décompte (le premier jeu du périmètre passe en CSH tôt).
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
(la largeur vient de HDR), la vitesse lente du CSL (les cycles se
comptent à 7,16 MHz), la BSRAM et le CD-ROM², le multitap. Un jeu qui
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
  run_hash stable d'une exécution à l'autre.
- **e2e synthétique** (`tests/test_pce_e2e.py`) : cartouche générée en
  Python (vecteurs, TAM, VDC, VCE), deux exécutions même hash, pixels du
  fond et du caractère vérifiés.

## Chantiers suivants

- La couche sprite : les gros glyphs du titre tombent juste, le texte
  sprite des bords de lignes montre des artefacts de découpe (limite des
  16 sprites de ligne ou fetch des demi-motifs 32 px — à trancher contre
  une référence).
- Le recompilateur HuC6280 → C (le modèle NES : régions, profils depuis
  le harness, interpréteur en repli) et la cible META — la VRAM de 64 Kio
  ne tient pas dans les 32 Kio de SRAM : il faudra la fenêtrer en flash
  comme la ROM Sega.
- Le PSG complet (LFO, balance, polynôme de bruit) et l'entrée audio du
  harness comparée à une référence.
