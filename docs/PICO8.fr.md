# PICO-8 sur la Gamebuino META

Statut au 2026-10-19. Les étiquettes disent d'où vient chaque affirmation :
**vérifié** (comparaison avec femto8 ou test automatique), **mesuré dans gm0**
(émulateur, pas la console), **estimé**.

## Ce que c'est

`m0recomp cart.p8.png --target meta` compile le Lua d'une cartouche PICO-8 en C,
puis en firmware pour la META. Ce n'est pas un interpréteur Lua embarqué : le
bytecode de la cartouche devient une fonction C par fonction Lua, sur un petit
runtime (`targets/pico8/runtime/`).

- **Front-end sur le PC** : le lecteur de cartouches (`.p8`, `.p8.png`) et le
  compilateur Lua de [femto8](https://github.com/benbaker76/femto8) (Lua 5.2
  PICO-8, nombres 16.16). femto8 reste un dépôt à part : `FEMTO8_DIR` (par
  défaut `../femto8`) ; sans lui, CMake construit m0boy sans PICO-8.
- **Générateur** (`recompiler/src/system/pico8/p8c.c`) : une fonction C par
  fonction Lua, les registres dans la pile de valeurs du runtime, les globales
  en variables C, les constantes dans la flash. Les fonctions Lua de l'API de
  femto8 (`add`, `del`, `all`, `foreach`, `count`, `deli`) sont compilées avec
  la cartouche quand elle s'en sert.
- **Runtime** : valeurs de 8 octets (nombre, booléen, chaîne, table, fonction),
  tables en tableau + nœuds chaînés (Lua 5.2), fermetures et valeurs montantes,
  tas de 18 Kio avec ramasse-miettes qui tasse les tampons (pas de
  fragmentation), dessin et son entiers (pas de virgule flottante).
- **Frontend META** (`targets/pico8/platform/p8_main.cpp`) : l'image 128×128
  affichée 1:1 au milieu de l'écran (fenêtre de colonnes du panneau réduite à
  128, 256 octets par ligne), D-pad, A = O, B = X, MENU relance la cartouche.
  Une erreur d'exécution arrête la cartouche et s'affiche à l'écran.

## Vérifié

- Écran identique à femto8, octet pour octet, toutes les 30 images : Celeste
  (1500 images, entrées aléatoires), Celeste Practice Mod (idem), une
  cartouche synthétique qui exerce tables, fermetures, chaînes et dessin
  (`tests/test_pico8_e2e.py`, empreinte de femto8 enregistrée dans le test ;
  le même test fait passer le ramasse-miettes à chaque allocation).
  Pour la comparaison, femto8 a tourné modifié (pas de veille, graine du
  hasard à 0, entrées scriptées, compteur d'images masqué de l'écran).
- Firmware META construit pour les deux Celeste (PlatformIO) et lancé dans gm0 :
  6000 images de jeu avec des entrées aléatoires sans erreur, son audible
  (hauteurs justes à l'analyse spectrale).

## Mesuré dans gm0 (pas sur la console)

- Celeste, firmware META : 18 s d'émulation = 474 images à 48 MHz, soit 26 images
  par seconde pour 30 demandées. gm0 ne modélise pas les 72 MHz du
  `META_OVERCLOCK` ; la console, à 72 MHz, devrait tenir 30 images
  (**estimé**, non mesuré).
- RAM 30,7 Kio sur 32 (tas 18 Kio, image 8 Kio, pile C 0,9 Kio au plus mesurée),
  flash 130 Kio sur 240.
- L'attente du SPI de l'écran (environ 20 % du temps) n'est pas recouverte par
  le calcul de l'image suivante.

## Limites connues

- **Mémoire** : les objets de Celeste occupent jusqu'à environ 16 Kio vivants
  en jeu (mesuré sur un hôte 32 bits). Une cartouche qui garde plus de données
  vivantes que le tas s'arrête sur « out of memory ». `P8_HEAP_BYTES`
  (`src/p8_config.h`) règle le tas ; la pile C (≈ 0,9 Kio utilisés) et le
  reste de la RAM limitent sa taille.
- **Non pris en charge** (la cartouche est compilée, l'appel arrête le jeu) :
  `peek`/`poke`/`memcpy`/`memset`/`reload`/`cstore`, `mset`/`sset`/`fset` (les
  données de la cartouche sont en flash), `fillp`, `tline`, `rrect`,
  `cartdata`/`dget`/`dset`, `split`, métatables, coroutines, `load`/`run`, `_G`
  et accès dynamique aux globales. Le compilateur signale les globales lues
  sans être définies.
- Texte : `print` dessine les caractères de base (4 ou 8 pixels de large) et
  `\n` ; pas de codes de contrôle P8SCII ni de polices personnalisées.
- Son : quatre canaux, les huit formes d'onde sauf la huitième (silence), les
  effets d'`sfx` ; boucles d'`sfx`, fondu et masque de `music` ignorés comme
  dans femto8. Le mixage est à 22050 Hz (femto8 : 44100) et n'est pas identique
  à l'échantillon près ; aucune comparaison à l'oreille n'a été faite.
- Pokitto : non pris en charge pour PICO-8.
- ESPboy (`--target espboy`) : construit avec PlatformIO (RAM 73,7 Kio sur 80, flash
  352 Ko) et lancé dans gm0 (Celeste affiche et joue, couleurs justes) ; la
  cartouche, la police et les tables sont en flash, lues par mots alignés
  (`p8_rom.h`). Tas de 28 Kio. Pas encore essayé sur la console, ni mesuré en
  vitesse ni en son.

## Utilisation

```bash
build/bin/m0recomp roms/pico8/Celeste/celeste.p8.png --target meta -o out/celeste
(cd out/celeste && pio run)           # .pio/build/meta/firmware.bin sur la carte SD
build/bin/m0recomp cart.p8.png --target desktop -o out/celeste-host   # harnais PC
cmake -G Ninja -S out/celeste-host/host -B out/celeste-host/host/build && ninja -C out/celeste-host/host/build
out/celeste-host/host/build/celeste_host --frames 600 --input "30:16:100,200:2:60" --shot image.ppm
build/bin/p8c cart.p8.png --info      # fonctions, globales, API manquante
build/bin/p8c cart.p8.png --lua       # le Lua tel que compilé
```

`--input` : « image:masque:durée » (bits 0 gauche, 1 droite, 2 haut, 3 bas,
4 O, 5 X). Variables du harnais : `P8_STATS=1` (mémoire par type), `P8_BINS=préfixe`
(écran brut toutes les 30 images), `P8_GC_EVERY=1`.
Options de construction utiles au débogage du firmware : `-DP8_ERROR_STATS`
(la page d'erreur détaille le tas), `-DP8_STACK_CHECK` (barre de la pile C en
haut de l'image), `-DP8_STOP_AFTER_US=n` (s'arrête à n µs et affiche le nombre
d'images, pour comparer des versions dans gm0).

## gm0

gm0 ne modélisait pas `DMAC->INTSTATUS` (offset 0x24) : le `DMAC_Handler` de
m0boy, qui s'en sert, tournait en boucle et tout firmware de la META (Game Boy,
SG-1000, PICO-8) restait bloqué sur son premier transfert d'écran. gm0 le
modélise maintenant (un bit par canal dont un drapeau activé est levé).
Ses captures de la META montrent le rouge et le bleu échangés avec le
`META_LCD_BGR` de m0boy ; le même firmware Game Boy ou SG-1000 est touché.
