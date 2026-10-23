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

Non vérifié : aucun jeu MSX1 réel n'a été essayé (la seule ROM MSX fournie, Aleste, est MSX2 ;
hors les jeux de la Super Game 150, qui tournent sur un autre matériel) ; ni
l'exécution sur console (META, Pokitto) ni sous gm0 ; le son contre un AY réel
(niveaux et cadence de l'enveloppe issus de la documentation de la
communauté) ; la vitesse (aucun relevé de cycles).
