# PICO-8 sur la Gamebuino META, la Pokitto et le PC

Statut au 2026-10-23. Les étiquettes disent d'où vient chaque affirmation :
**vérifié** (comparaison avec femto8 ou test automatique), **mesuré dans gm0**
(émulateur, pas la console), **estimé**.

## Ce que c'est

`m0recomp cart.p8.png --target meta` (ou `--target pokitto`) compile le Lua d'une
cartouche PICO-8 en C,
puis en firmware pour la console. Ce n'est pas un interpréteur Lua embarqué : le
bytecode de la cartouche devient une fonction C par fonction Lua, sur un petit
runtime (`targets/pico8/runtime/`).

`--target pc` produit aussi une fenêtre SDL2 de 160×128 : l'image PICO-8
occupe les 128 colonnes centrales. Le projet se construit avec CMake et Ninja ;
la fixture synthétique a été générée, compilée et lancée sans fenêtre dans
`tests/test_pc_target.py`. Les états PC exigent un `sd.img` contenant le fichier
`.ST8`, comme les autres projets PC à carte virtuelle.

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
- **Frontend** (`targets/pico8/platform/p8_main.cpp`, les trois consoles) :
  l'image 128×128 affichée 1:1 au milieu de l'écran (sur la META, fenêtre de
  colonnes du panneau réduite à 128, 256 octets par ligne ; sur la Pokitto,
  colonnes 46..173, lignes 24..151, chaque ligne poussée par
  `pokitto_p8lcd.c`), D-pad, A = O, B = X, MENU relance la cartouche (sur la
  Pokitto : une impulsion sur C, maintenue à mi-seconde elle ouvre le menu).
  Une erreur d'exécution arrête la cartouche et s'affiche à l'écran.
- **Save states** (0.10.34 sur la META et l'ESPboy, 0.10.35 sur la Pokitto) :
  lignes Etat / Sauver / Charger
  du menu, entre deux images. `p8_state_io` sérialise le tas et ses index,
  l'état du dessin et l'écran, les boutons, le générateur aléatoire, les
  canaux audio et toutes les globales que la cartouche peut changer
  (`p8_gen_state_globals`, émise par p8c). Le tas contient des pointeurs
  absolus : un état ne se recharge que dans le firmware qui l'a écrit,
  reconnu par une empreinte de son code (`p8_state_identity`, FNV-1a de
  0x4000 à `__etext` sur la META, de la zone irom0 sur l'ESPboy, de 0x0 à
  `_sidata` sur la Pokitto) qui entre
  dans le format. Fichier `<NOM>.ST8` sur la carte (créé par `compile.sh`,
  10 emplacements ; sur la Pokitto, cherché à la racine et dans les dossiers
  de premier niveau), dans la flash sur l'ESPboy (6 emplacements pour
  Celeste). **Vérifié** : sur le harnais (sans PIE, mêmes adresses à chaque
  lancement), un état sauvé à l'image 300 puis rechargé finit à l'image 600
  sur l'empreinte de femto8, ramasse-miettes forcé compris
  (`tests/test_pico8_e2e.py`). **Mesuré dans gm0** : Celeste sur META et
  ESPboy, sauver, jouer, charger, « Etat charge » et l'image de la
  sauvegarde ; la pile C mesurée en jeu est donnée plus bas.
- Le menu (META) dessine une ligne de 160 pixels à la fois dans les tampons
  de l'image (640 octets rendus à la pile) et rouvre les 160 colonnes du
  panneau le temps de s'afficher (il était brouillé depuis 0.10.28) ; sur
  l'ESPboy ses lignes sont réduites à 128 colonnes (il était coupé). Sur la
  Pokitto ses lignes sortent telles quelles (fenêtre Game Boy du panneau) et
  l'écran est effacé à la fermeture.
- **ESPboy** : tas de 22 Kio et pile de continuation de 6 Kio en `.bss` (le
  code PICO-8 demande environ 1 Kio de pile C). `compile.sh` traite les
  cartouches (META, Pokitto et ESPboy).

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

- Celeste Practice Mod, firmware META, 6000 images d'affichage (100,46 s
  émulées) à 48 MHz : 2757 images de jeu avant optimisation, 2993 après, soit
  +8,6 % et environ 30,0 images par seconde hors démarrage (27,6 avant) pour
  30 demandées. gm0 ne modélise pas les 72 MHz du `META_OVERCLOCK` ; la
  console, à 72 MHz, a plus de marge (**estimé**, non mesuré).
- Celeste (cartouche d'origine), même mesure après optimisation : 2993 images,
  soit environ 30,0 par seconde hors démarrage.
- RAM 30,5 Kio sur 32 (tas 18 Kio, image 8 Kio, pile C 1 056 octets au plus
  mesurée en jeu sur 1 544 libres), flash 178 Kio sur 240 pour Celeste Practice Mod.
- Le transfert SPI de l'écran reste d'environ 11,7 ms par image (24 MHz) et
  n'est pas recouvert par le calcul de l'image suivante ; c'est la prochaine
  réserve de temps si le calcul devient plus court.
- Le code Lua généré est compilé en `-O2` par défaut ; `-DP8_GEN_SIZE` le
  repasse en `-Os` pour une cartouche qui ne tiendrait plus en flash. Le
  runtime lit les rangées de sprites par mot, copie d'un mot les sprites
  entièrement opaques, remplit l'écran par mots de 32 bits et avance dans les
  sprites mis à l'échelle sans division par pixel. Ces changements ne modifient
  pas l'image (même hash que femto8, y compris sous `P8_GC_STRESS`).

## Limites connues

- **Mémoire** : les objets de Celeste occupent jusqu'à environ 16 Kio vivants
  en jeu (mesuré sur un hôte 32 bits). Une cartouche qui garde plus de données
  vivantes que le tas s'arrête sur « out of memory ». `P8_HEAP_BYTES`
  (`src/p8_config.h`) règle le tas ; la pile C (1 056 octets utilisés au plus
  en jeu sur 1 544 libres pour Celeste Practice Mod) et le reste de la RAM limitent
  sa taille. `-DP8_GEN_SIZE` rend environ 22 Kio de flash et 128 octets de
  pile au prix d'un code Lua plus lent.
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
- Pokitto (`--target pokitto`) : firmware construit (gcc et clang, sous le
  loader à 0x39000, checksums et vecteurs du LPC vérifiés par le test,
  `tests/test_pico8_e2e.py`), tas de 18 Kio et anneau audio dans la banque
  RAM1 (POKITTO_RAM_BANKS 1 ; 29,7 Kio de RAM principale sur 32 au rapport
  du build, 175 032 octets de firmware sous le loader — **estimé**).
  **Mesuré dans gm0** : l'image 1:1 centrée, les
  boutons, le menu d'options (C maintenu), sauver puis charger un état
  (le `.ST8` à côté du jeu, dans un dossier de premier niveau de la carte —
  la recherche FAT de la Pokitto ne descend pas plus loin, comme pour les
  autres cœurs). Pas encore essayé sur la console : ni l'écran, ni le son.
- ESPboy (`--target espboy`) : construit avec PlatformIO (RAM 75,9 Kio sur 80,
  firmware 383 Ko) et lancé dans gm0 (Celeste affiche et joue, couleurs
  justes) ; la
  cartouche, la police et les tables sont en flash, lues par mots alignés
  (`p8_rom.h`). Tas de 28 Kio. Pas encore essayé sur la console, ni mesuré en
  vitesse ni en son.

## Utilisation

```bash
build/bin/m0recomp roms/pico8/Celeste/celeste.p8.png --target meta -o out/celeste
(cd out/celeste && pio run)           # .pio/build/meta/firmware.bin sur la carte SD
build/bin/m0recomp cart.p8.png --target pokitto -o out/celeste-pokitto
(cd out/celeste-pokitto && cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build)
                                      # build/firmware.bin sur la carte SD
build/bin/m0recomp cart.p8.png --target desktop -o out/celeste-host   # harnais PC
cmake -G Ninja -S out/celeste-host/host -B out/celeste-host/host/build && ninja -C out/celeste-host/host/build
out/celeste-host/host/build/celeste_host --frames 600 --input "30:16:100,200:2:60" --shot image.ppm
build/bin/p8c cart.p8.png --info      # fonctions, globales, API manquante
build/bin/p8c cart.p8.png --lua       # le Lua tel que compilé
```

`--input` : « image:masque:durée » (bits 0 gauche, 1 droite, 2 haut, 3 bas,
4 O, 5 X). `--input-file fichier` lit les mêmes segments dans un fichier —
celui qu'écrit la fenêtre PC avec `M0_RECORD_INPUT=fichier` (un segment se
ferme à chaque changement de boutons ; un redémarrage ou un état sauvegardé
du menu y met fin) et que rejoue aussi la fenêtre avec `M0_INPUT=fichier`.
Variables du harnais : `P8_STATS=1` (mémoire par type), `P8_BINS=préfixe`
(écran brut toutes les 30 images), `P8_GC_EVERY=1`.
Options de construction utiles au débogage du firmware : `-DP8_ERROR_STATS`
(la page d'erreur détaille le tas), `-DP8_STACK_CHECK` (barre de la pile C en
haut de l'image), `-DP8_STOP_AFTER_US=n` (s'arrête à n µs et affiche le nombre
d'images, pour comparer des versions dans gm0). `-DP8_GEN_SIZE` compile le
code Lua généré en `-Os` au lieu de `-O2` : environ 22 Kio de flash et 128
octets de pile en moins, au prix d'environ 10 % sur `p8_frame` pour Celeste
Practice Mod.

## gm0

gm0 ne modélisait pas `DMAC->INTSTATUS` (offset 0x24) : le `DMAC_Handler` de
m0boy, qui s'en sert, tournait en boucle et tout firmware de la META (Game Boy,
SG-1000, PICO-8) restait bloqué sur son premier transfert d'écran. gm0 le
modélise maintenant (un bit par canal dont un drapeau activé est levé).
Ses captures de la META montrent le rouge et le bleu échangés avec le
`META_LCD_BGR` de m0boy ; le même firmware Game Boy ou SG-1000 est touché.
