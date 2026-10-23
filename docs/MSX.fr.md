# MSX1 : cartouches ROM

`--system msx` (extensions `.mx1`, `.msx`, ou `.rom` commençant par `AB`)
recompile une cartouche MSX1 vers la META, la Pokitto ou le PC (`desktop`).
Ce n'est pas un émulateur de MSX complet : c'est le runtime SG-1000 (Z80,
TMS9918A, mêmes frontends) avec la carte mémoire, les ports, le PSG et un BIOS
simulé du MSX (`targets/msx/runtime/`, activé par `SG_MSX`). Le firmware SG-1000
reste identique octet pour octet (comparé sur une cartouche synthétique,
Pokitto, avant et après).

## Ce qui est pris en charge

- **Cartouches** : ROM de 1 octet à 1 Mio avec l'en-tête `AB` et une routine
  INIT dans la cartouche (`targets/common/runtime/m0_msx_layout.h`, une seule
  règle pour le recompilateur et le runtime). Jusqu'à 32 Kio, ROM non paginée :
  une ROM de 16 Kio est en 4000-7FFF, ou en 8000-BFFF si INIT s'y trouve, au-delà
  en 4000-BFFF. Au-delà de 32 Kio, un mapper (la ROM vue en unités de 8 Kio à
  travers quatre fenêtres 4000/6000/8000/A000) : **ASCII8**, **ASCII16**,
  **Konami** et **Konami SCC** (sans le SCC). Le mapper est deviné par les
  écritures `LD (nn),A` aux registres de banque (ASCII16 en cas d'égalité) ;
  `--msx-mapper plain|ascii8|ascii16|konami|konami-scc` (et `msx_host --mapper`)
  le force. Ces ROM sont recompilées comme la Game Gear (un emplacement est une
  page et une adresse, le code revient au répartiteur quand une fenêtre change).
  Une ROM sans en-tête ou sans INIT (extension BASIC) est refusée.
- **ROM en flash** : brute si elle tient ; sinon (la META ou la Pokitto avec
  une ROM de 256 Kio) compressée par secteurs de 512 octets (LZSS ou brut, comme
  le pool de la Game Gear) et décodée à la demande dans un cache de 3 (META) ou
  4 secteurs. Le streaming depuis la carte SD n'est pas disponible pour le MSX :
  une ROM qui ne tient pas compressée est refusée.
- **Mémoire** : quatre pages de 16 Kio dans quatre slots primaires choisis par
  le PPI (port A8) : slot 0 BIOS, slot 1 cartouche, slot 2 vide, slot 3 RAM.
  Au départ : page 0 BIOS, la page de la cartouche sur le slot 1, RAM au-dessus.
  Pas de slots étendus.
- **RAM** : 16 Kio en C000-FFFF sur le PC, **8 Kio en E000-FFFF** sur la META et
  la Pokitto (la SRAM n'en contient pas plus avec la VRAM de 16 Kio). Un jeu qui
  utilise C000-DFFF ne fonctionnera pas sur console (les lectures donnent FF).
- **Vidéo** : TMS9918A (ports 98/99), les rendus du runtime SG-1000 : Graphics I
  et II, texte, multicolore, sprites. Interruption de 60 Hz.
- **Son** : AY-3-8910 (ports A0-A2), trois voix, bruit et enveloppe
  (`msx_ay.c`). Pas de SCC, ni MSX-MUSIC.
- **Entrées** : la croix et les deux boutons de la console valent le joystick 1
  (registre 14 du PSG, `GTSTCK`, `GTTRIG`) et les touches du clavier lues par le
  PPI/`SNSMAT` (curseurs et espace ; le bouton de pause vaut RETURN).
- **BIOS simulé** (`msx.c`) : le saut table de 0000 à 0165 renvoie vers des
  routines C par un `OUT (n),A ; RET` ; le gestionnaire d'interruption (0038)
  est du vrai Z80, il appelle les crochets H.KEYI et H.TIMI en RAM, range le
  statut du VDP en STATFL et compte JIFFY. Routines : RDSLT, WRSLT, CALSLT (le
  slot n'est pas commuté), ENASLT, CALLF, RSLREG, WSLREG, DCOMPR, DISSCR,
  ENASCR, WRTVDP, RDVRM, WRTVRM, SETRD, SETWRT, FILVRM, LDIRMV, LDIRVM, RDVDP,
  CHGMOD, CHGCLR, CLRSPR, INITXT, INIT32, INIGRP, INIMLT, SETTXT, SETT32,
  SETGRP, SETMLT, CLS, GICINI, WRTPSG, RDPSG, GTSTCK, GTTRIG, GTPAD, GTPDL,
  SNSMAT, CHSNS, CHGET. Les autres points d'entrée sont un `RET`. Les variables
  système usuelles (RGnSAV, STATFL, FORCLR, BAKCLR, BDRCLR, SCRMOD, NAMBAS...,
  JIFFY, RAMADn, crochets) sont initialisées comme après le démarrage du BIOS.

## Interruption, vitesse et Gradius 2 (2026-10-23)

- La routine d'interruption (0038) sauvegarde **tous** les registres, IX et IY
  compris, comme le BIOS (C-BIOS `keyint`). Sans cela, Gradius 2 — qui exécute sa
  logique dans le crochet H.TIMI, avec IX pour pointeur d'objet — corrompait ses
  sprites, écrasait sa RAM puis tombait dans un toboggan de NOP : « ça rame puis
  ça reset ». Vérifié : 6 graines d'entrées aléatoires (directions possibles
  seulement), 20 000 images chacune, sans plantage ; avant, plantage vers l'image
  550-670 à chaque graine. `tests/test_msx_runtime.c` en garde un test (IX/IY
  rendus intacts).
- Un `CALL` d'un code recompilé vers une routine du BIOS simulé (table
  `targets/common/runtime/m0_msx_bios.h`) appelle la routine directement, sans
  passer par le stub interprété (hors build précis, où l'interpréteur et le code
  natif doivent donner le même résultat à chaque instruction).
- AY : une voix muette ou à bruit et enveloppe inutilisés ne coûte plus de
  comptage, et les voix à tonalité seule se génèrent par blocs (même son, octet
  pour octet).
- Budget de flash : le générateur compte désormais le runtime MSX (62 Kio) et
  39 octets par instruction compilée, et donne à la ROM brute tout ce qui reste.
  `--msx-pack` force la ROM compressée : mesuré sur Gradius 2 (gm0, estimation) c'est
  plus lent (28 % du temps à décompresser contre 1675 instructions compilées de plus).
- Mesure (gm0, estimation, pas la console) : Gradius 2 en jeu avec des entrées
  scriptées, 2954 images du jeu en 3600 images gm0 (82 % du temps réel) ; reste
  de l'ordre de 27 % pour le rendu des lignes et 20 % pour l'interpréteur (le code
  de jeu au-delà des ~1300 instructions que la flash permet de compiler).
- Garde de pile des firmwares MSX : 1400 octets réservés au runtime (au lieu de
  2560 : le maximum observé sous la garde est de 672 octets), le reste sert aux
  appels imbriqués du code recompilé.

## Le BIOS : licence

Le BIOS d'origine est protégé et n'est ni utilisé ni fourni. Le BIOS est
simulé (ci-dessus) ; la seule donnée tierce est la **police** des modes texte,
reprise de [C-BIOS](https://cbios.sourceforge.net) 0.28 (BIOS libre sous licence
BSD à deux clauses, redistribuable avec des émulateurs) : la table des 256
glyphes de `font_us.asm` de Eric Boon, `targets/msx/runtime/msx_font.h`, qui
garde l'avis de copyright et la licence. `CHPUT` (écran 0 et 1 : retour chariot,
saut de ligne, effacement, défilement), `POSIT`, `INITXT` et `INIT32` s'en
servent. C-BIOS lui-même (du Z80 à exécuter à la place du BIOS simulé) n'est pas
embarqué : il demande 64 Kio de RAM pour sa détection de mémoire.

## Ce qui manque

Les modes graphiques n'ont pas de routine de texte (`GRPPRT`), et tout ce qui
touche à la cassette, à l'imprimante, aux disques, au BASIC et au clavier
complet (hors les touches ci-dessus) est absent. **MSX2 et plus** (VDP V9938 et
ses 128 Kio de VRAM, modes 4 à 8, RAM paginée, horloge) ne sont pas pris en
charge : la VRAM de 128 Kio ne tient pas dans la SRAM de la META (32 Kio).
`Aleste (1988)(Compile)` en est un : il écrit les registres R#14 et R#15 du
V9938. Un jeu qui appelle une routine du BIOS non listée, ou qui attend une
touche autre que celles-là, ne fonctionnera pas.

## Utilisation

```bash
build/bin/msx_host jeu.mx1 --frames 3000 --coverage jeu.cov   # profil (interpréteur)
build/bin/m0recomp jeu.mx1 --target meta --coverage jeu.cov -o out/msx/meta/jeu
pio run -d out/msx/meta/jeu
```

`--meta-precise` donne le même `run_hash` que `msx_host --interp`, comme pour
le SG-1000. Le profil est un fichier `# m0boy.msx.coverage.v2`. Le
`compile.sh` ne traite pas encore les `.mx1` ; l'ESPboy n'est pas câblé.

## Validation

Évidence collectée :

- `tests/test_msx_e2e.py` (cartouche synthétique `tests/msx_fixture.py`, rien
  d'un jeu) : native précise = interpréteur (hash, image, audio) pour
  `desktop`, `meta` et `pokitto`, budget de code quasi vide (repli sur
  l'interpréteur), build par défaut ; l'image n'est pas vide et l'AY produit du
  son ; les ROM sans en-tête sont refusées.
- Le firmware META (PlatformIO) et le firmware Pokitto de la cartouche
  synthétique se construisent : META RAM 29 204 / 32 768 octets, flash
  75 040 octets ; Pokitto 67,7 Kio.
- Registres, tables VRAM, joystick et clavier, slots, JIFFY et crochet H.TIMI
  de la cartouche synthétique relus dans l'état de la machine.

- Cartouches synthétiques de 256 Kio pour chacun des quatre mappers
  (`mapper_fixture`) : registres de banque, appels dans les fenêtres paginées,
  lecture de données ; native précise = interpréteur sur `desktop`, `meta` et
  `pokitto` (ROM compressée en flash sur les deux consoles). Firmware META
  construit : RAM 30 652 / 32 768 octets (ROM compressée, cache de 3 secteurs),
  flash 98 940 octets.
- Texte : `CHPUT`, `POSIT` et la police chargée (`tests/test_msx_runtime.c`).

Non vérifié : aucun jeu MSX1 réel n'a été essayé sur console (la seule ROM MSX
fournie, Aleste, est MSX2 ; hors les jeux de la Super Game 150, qui tournent sur
un autre matériel) ; l'exécution sur META et Pokitto ; le son contre un AY réel
(niveaux et cadence de l'enveloppe issus de la documentation de la
communauté) ; la vitesse sur console (les chiffres gm0 ci-dessous sont des
estimations, pas des relevés de cycles).

## BIOS, son et rendu (0.10.40)

- **Contrat des registres du BIOS** : les jeux s'appuient sur les registres de
  sortie du vrai BIOS et sur les variables système. INIGRP/INITXT/SETGRP
  construisent les registres du VDP à partir de `TXTNAM…MLTPAT` (F3B3 + 10·mode),
  LDIRVM sort avec HL = destination VRAM, DE = source + n, BC = 0x0098, l'ISR
  sauve IX/IY, NEWKEY/OLDKEY valent 0xFF au repos. Méthode de validation : la
  VRAM d'openMSX (`debug read_block "VRAM"`) comparée à `--dump-vram`.
- **Son** : AY-3-8910, SCC (Konami SCC, 5 voix, 32 pas) et clic du PPI mélangés
  à 22 050 Hz ; MSX-MUSIC non émulé.
- **Rendu** : le rendu en assembleur du SG-1000 sert aussi au MSX (pool de
  `SG_MIX_SLOTS` lignes de mélange au lieu de la table de 4,6 Ko).
- **Mesure** : Gradius 2 sur META, 60 images de jeu par seconde (compteur
  `machine.frames` contre `ticks`, 90 s de jeu) ; gm0 (48 MHz) donne ~93 %, c'est
  une estimation, pas une mesure de la console. Les images affichées (saut vidéo
  du pacing) ne sont pas mesurées.
- `--msx-pack` : ROM en secteurs LZSS dans la flash ; plus lent sur Gradius 2.

## Chemins rapides de rendu et d'audio (0.10.41)

- **Rendu** : dans `sg_render_asm.S`, une tuile de motif nul
  (`pa == pb == 0`) et une paire dont les deux lignes sont identiques et de
  motif nul recopient cinq fois le premier mélange au lieu d'en calculer cinq.
  Gradius 2 en jeu sous gm0 (3 600 images, mêmes entrées des deux côtés) :
  `sg_pair_g2` 8,77 s → 6,12 s (−30 %), `sg_tiles_g2` 3,98 s → 3,20 s (−20 %),
  soit ~3,4 s de CPU émulé en moins. Le total reste 59,47 s : la boucle est
  cadencée à 60 Hz et le temps gagné passe dans les attentes.
- **Audio** : `sg1000_audio_render` copie le ring par segments (`memcpy`) au
  lieu d'un échantillon à la fois ; une écriture AY identique (sauf R13, qui
  relance l'enveloppe) ou SCC identique, ou sur une voix muette, ne découpe
  plus le bloc en cours. Même mesure : `produce` 3,24 s → 2,67 s et le rendu
  0,70 s → 0,45 s. Le WAV du harnais hôte est identique bit pour bit avant et
  après.
- **Validation du rendu asm** : build `-DSG_SELFTEST`, `sg_selftest_result` lu
  dans la RAM sous gm0 vaut 1 (aucune ligne différente entre l'assembleur et
  le C) pour `SG_PANEL_SWAP` 0 et 1. Une des passes du test force des motifs
  nuls et une couleur constante (`0xE4`) pour couvrir les deux copies rapides,
  que la VRAM aléatoire n'atteint presque jamais.
- **Validation en jeu** : Gradius 2 arrêté à la même image du jeu (compteur
  invité 900, après l'appui sur A) avec le firmware d'avant et celui-ci :
  captures identiques (md5 `ea2ddaab…`), et la RAM du jeu, la VRAM et les
  registres du VDP sont identiques ; seuls les compteurs d'audio et de temps,
  la pile et les pointeurs de code diffèrent.
