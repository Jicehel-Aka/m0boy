# CPU, console d’origine et destination

La sélection d’un jeu comporte désormais trois identités indépendantes :

| Axe | Game Boy | SG-1000 |
| --- | --- | --- |
| Console d’origine (`--system`) | `gameboy` | `sg1000` |
| CPU requis (`--cpu`, contrôle facultatif) | `sm83` | `z80` |
| Destination (`--target`) | `desktop`, `meta`, `pokitto` | `desktop` (harness), `meta`, `pokitto` |
| État actuel | Pipeline existant | Code Z80 recompilé, runtime TMS9918A/SN76489, cartouches sans mapper |

`--target sg1000` n’est pas une destination : la SG-1000 est la machine
dont on traduit le programme. META et Pokitto restent les machines sur
lesquelles les firmwares Game Boy s’exécutent.

## Organisation

- `recompiler/include/m0boy/memory.h` : interface `ByteReader`. Un décodeur
  reçoit des octets et une adresse, sans connaître le format de cartouche.
- `recompiler/include/m0boy/system.h`, `recompiler/src/system.cpp` : registre
  des CPU, des consoles d’origine et de leurs capacités de génération.
- `recompiler/include/m0boy/cpu/encoding.h` : codes de registres, champs
  `x/y/z/p/q`, lecture little-endian et calcul des branches relatives communs.
- `recompiler/src/cpu/z80/`, `include/m0boy/cpu/z80/` : désassemblage Z80,
  préfixes `CB/ED/DD/FD`, registres indexés, timings et métadonnées de contrôle
  de flux. La bibliothèque `m0boy_cpu_z80` ne dépend pas des cartouches.
- `recompiler/src/cpu/sm83/`, `include/recompiler/cpu/sm83/` : décodage SM83.
  La bibliothèque CMake `m0boy_cpu_sm83` se construit sans le parseur Nintendo.
- `recompiler/src/system/gameboy/` : cartouche Game Boy, analyse des banques,
  suivi des mappers et balayage du code par banque.
- `recompiler/src/system/sg1000/`, `include/m0boy/sg1000*.h` : cartouche
  SG-1000, recompilation Z80 → C (`emitter.cpp`) et génération des projets.
- `m0boy_frontends` : chargement et métadonnées des consoles d’origine.
- `gbrecomp_core` : analyse, IR et générateurs Game Boy existants.
- `targets/meta/` : runtime Game Boy et pilotes des destinations existantes.
- `targets/sg1000/runtime/` : runtime SG-1000 commun au code recompilé et à
  l’interpréteur Z80 de repli (bus, événements, TMS9918A, SN76489,
  `sg_gen.h` pour le code généré). `targets/sg1000/platform/` : frontend des
  consoles, qui réutilise les pilotes écran, son, horloge, démarrage et
  boutons de META/Pokitto. `targets/sg1000/host/` : harness et lecteur SDL.

Les anciens includes `recompiler/rom.h` et `recompiler/decoder.h`, le namespace
`gbrecomp` et l’exécutable `gbrecomp` restent utilisables. Le nouvel exécutable
`m0recomp` utilise exactement la même CLI. `compile.sh` traite aussi les
`.sg` ; le frontend Node reste limité au Game Boy.

L’émetteur Z80 reprend la structure de l’émetteur Game Boy (régions,
répartition, vivacité des drapeaux, cycles par lots, synchronisation aux
têtes de boucle) sans la partager encore : une couche commune (instruction
décodée, flux, cibles, cycles, drapeaux lus et écrits) est l’étape suivante
pour unifier les deux. Le runtime `gbl` reste propre à la Game Boy.

## Utilisation

```bash
build/bin/m0recomp --list-systems
build/bin/m0recomp jeu.sg --info
build/bin/m0recomp jeu.sg --disasm --limit 32
build/bin/m0recomp jeu.bin --system sg1000 --cpu z80 --info
build/bin/m0recomp jeu.gb --system gameboy --target meta -o output/jeu
build/bin/sg1000_host jeu.sg --frames 300 --coverage output/jeu-sg.cov
build/bin/m0recomp jeu.sg --target pokitto --coverage output/jeu-sg.cov -o output/jeu-pokitto
cmake -G Ninja -S output/jeu-pokitto/host -B output/jeu-pokitto/host/build
ninja -C output/jeu-pokitto/host/build
cmake -G Ninja -S output/jeu-pokitto -B output/jeu-pokitto/build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake
ninja -C output/jeu-pokitto/build
```

Le mode `auto` reconnaît `.sg` et `.gb/.gbc/.sgb`, sans distinction de casse.
Les extensions inconnues et les dossiers conservent le choix Game Boy
historique. Pour un fichier `.bin` SG-1000, il faut donc indiquer `--system
sg1000`. `.sgb` désigne toujours une cartouche Game Boy, pas une SG-1000.
La sélection du CPU contrôle la cohérence avec la console, elle ne change
pas son matériel.

Une cartouche SG-1000 n’a pas besoin d’un en-tête Nintendo. Le profil
accepte de 1 à 48 Kio de ROM brute, sans mapper. Les tailles supérieures sont
refusées avec un diagnostic explicite. Les cartes en puissance de deux sont
reflétées dans `0000–BFFF` ; les dumps de 40 et 48 Kio sont lus tels quels,
`FF` au-delà. Une cartouche de 32 Kio au plus peut porter 8 Kio de RAM en
`8000–BFFF` (The Castle) : le profilage la détecte (écriture dans cette zone,
ligne `# cart-ram` de la couverture) et le projet l’active alors.

Le profil déclare le reset Z80 à `0000` et 1 Kio de RAM système. La
[carte mémoire SG-1000](https://www.smspower.org/Development/MemoryMap)
place la cartouche dans `0000–BFFF` et la RAM dans `C000–C3FF`, avec des
miroirs jusqu’à `FFFF`.

Le désassemblage (`--disasm`) est un balayage linéaire : des données peuvent
apparaître comme des instructions. `--limit` limite le nombre d’instructions
affichées.

## Recompilation Z80

Le code SG-1000 est recompilé, comme celui de la Game Boy : le générateur
(`recompiler/src/system/sg1000/emitter.cpp`) reprend l’architecture de
l’émetteur META.

- **Sélection** : les instructions jouées pendant le profilage (les plus
  exécutées d’abord), puis celles qu’elles atteignent statiquement (suites,
  sauts, appels, `RST`, vecteurs `0000/0038/0066`), dans la limite de la
  flash (32 octets estimés par instruction, `--meta-code-budget` pour la
  fixer). L’exploration statique s’arrête sur le remplissage (quatre octets
  `00` ou `FF`). Le code en RAM et le reste passent par l’interpréteur.
- **Régions** : les instructions voisines forment des fonctions C, avec des
  `goto` internes et un `switch` sur `pc` à l’entrée. `CALL` devient un appel
  C suivi d’un contrôle du retour ; les autres sorties posent `pc` et
  enchaînent sur la région cible ou rendent la main au répartiteur, qui
  cherche `pc` dans la table d’entrées triée (cache direct devant).
- **Drapeaux** : une analyse de vivacité par région ne calcule les drapeaux
  que lorsqu’une instruction suivante peut les lire (sinon, l’opération seule ;
  après `ADD HL,rr`, la seule retenue quand elle suffit).
- **Temps** : cycles et rafraîchissement `R` sont ajoutés par lots, avant tout
  ce qui les observe (ports, `LD A,R`, synchronisation, transferts). `SYNC`
  aux têtes de boucle, à l’entrée des régions et entre les itérations des
  instructions de bloc traite les événements et prend les interruptions.
- **Traduction** : les instructions courantes sont écrites en C (chargements,
  ALU, `INC/DEC`, `ADD HL`, rotations, `BIT/SET/RES`, piles, sauts, ports,
  `LDIR/LDDR/OTIR/OTDR` en boucles) ; les rares passent par la sémantique de
  l’interpréteur (`z80_ops.h`, opérandes constants). Accès mémoire,
  pile et drapeaux complets sont des fonctions hors ligne (taille de flash),
  les adresses constantes sont résolues à la génération (ROM repliée, RAM
  indexée directement).
- **EI** : l’instruction qui suit `EI` est émise en ligne, puis le délai prend
  fin et une synchronisation prend l’interruption en attente (la fenêtre
  `EI / NOP / DI` ne coûte rien de plus). Une reprise pendant un délai passe
  par l’interpréteur.

Le runtime (`targets/sg1000/runtime/`) compte le temps en T-states avec deux
événements par trame (VBlank en ligne 192, fin de trame en ligne 262). Les
lignes sont rendues à la demande, quand le programme touche le VDP ou au
VBlank, avec l’état VDP de ce moment : aucun test par ligne dans le code
généré, et une ligne voit toujours les écritures faites avant sa fin. Le PSG
est rattrapé de la même façon, avant chaque écriture et en fin de trame.

Le rendu des consoles réduit 256×192 en 160×120 (META) par un filtre boîte
exact, poids en huitièmes : chaque pixel couvre 1,6 pixel source, soit, pour
les cinq pixels d'une tuile, (5,3) (2,5,1) (4,4) (1,5,2) (3,5). Une tuile n'a
que deux couleurs : chaque pixel vaut le mélange encre/fond au poids des bits
d'encre, lu dans une table de 9 mélanges par octet de couleur, au format du
panneau (`mix`, reconstruite quand la couleur de fond change ; la palette du
TMS9918 est fixe). Verticalement, deux lignes source qui tombent sur la même
ligne d'écran sont rendues ensemble : poids moyen quand leurs couleurs sont
égales (une seule lecture), sinon moyenne des deux mélanges. Les tuiles sous
sprites prennent la boîte exacte 8×2 (couleurs étalées, une table de 16 avec
le fond) ; les modes texte/multicolore moyennent deux lignes filtrées. Les
boucles de tuiles des modes Graphics I et II sont en Thumb-1, exécutées en
SRAM (`sg_render_asm.S`) ; `SG_NO_RENDER_ASM` garde le C, et un firmware
construit avec `SG_SELFTEST` compare les deux sur VRAM aléatoire
(`sg_selftest_result` = 1 + lignes différentes, lu par gm0 ou un débogueur).
Les masques de sprites par ligne suivent chaque écriture d'un Y de sprite
(déplacement de ses bits) ; terminateur, taille ou table les reconstruisent.
Le rendu de référence (256 pixels) sert aux captures et aux tests de l'hôte.
Les projets avec RAM de cartouche (`SG_LOW_RAM`) remplacent la table des
mélanges par un cache de 16 et les masques par ligne par des masques par bande
de 8 lignes, sans assembleur. Le cadenceur peut sauter jusqu'à deux
présentations de suite et rattrape jusqu'à six images de retard : le jeu garde
sa vitesse.

Le PSG produit ses échantillons par blocs : un passage de Bresenham par bloc
donne les ticks de chaque échantillon, partagés par les quatre canaux ; un
canal muet n'avance que son compteur (une division). La sortie est identique
au calcul échantillon par échantillon.

## Vérification

Le harness généré (`host/`) accepte `--interp`, `--frames`, `--coverage`,
`--buttons`, `--start-frame`, `--pause-frame`, `--shot` et `--wav` (PCM mono
8 bits, 22050 Hz). Les bits de `--buttons` sont Haut, Bas, Gauche, Droite,
bouton 1 et bouton 2 (bits 0 à 5) ; `--start-frame` tient le bouton 1 pendant
dix images. Le `run_hash` couvre registres, cycles, RAM, VRAM, état VDP/PSG,
RAM de cartouche et image ; chaque ligne du rendu des consoles est comparée
au rendu de référence (`out_mismatch`).

`--meta-precise` synchronise avant chaque instruction et calcule tous les
drapeaux : le code recompilé doit alors donner exactement le `run_hash` de
l’interpréteur. Le 13 octobre 2026, c’était le cas pour les 68 cartouches
d’une collection SG-1000 (900 images, pause à l’image 500), et le rendu
160 pixels égalait le rendu de référence sur toutes les lignes. Les tests
CTest le vérifient sur un programme synthétique (repli sur un budget presque
vide compris) ; le noyau Z80 passe ZEXDOC et 66 tests ZEXALL sur 67 (seul
`BIT n,(HL)` diffère : ses bits X/Y viennent de MEMPTR, non modélisé).

Le build normal (synchronisation aux têtes de boucle, drapeaux utiles
seulement) peut placer une interruption quelques instructions plus tard ; il
n’est donc comparé qu’à l’exécution (images, son, absence de faute).

Chaque projet possède un seul système de destination. Pour META, la
génération produit `platformio.ini` : `pio run -d output/jeu-meta`. Pour
Pokitto, `cmake -G Ninja -S output/jeu-pokitto -B output/jeu-pokitto/build
-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake` puis Ninja, avec le checksum de
vecteurs LPC vérifié après liaison ; `-DSG_COMPILER=clang` compile le C/C++ avec
Clang (`toolchain/arm_clang.py`). `compile.sh` fait tout cela, avec
`FIRMWARE_COMPILER=gcc|clang`. L’image garde le rapport 4:3 : 160×120 au
centre du META, 200×150 au centre du Pokitto. MENU/C fournit l’impulsion NMI
de la touche Pause.

## Vitesse

Mesurée dans gm0 (émulation Cortex-M0+ au cycle, attentes flash et DMA LCD
comprises) : images SG-1000 produites pendant 30,1 s émulées, 59,92 images/s
étant la vitesse NTSC. Ce sont des mesures d’émulateur, pas des mesures sur
console.

Le 13 octobre 2026, sur les 68 cartouches d'une collection SG-1000, firmwares
META construits par `compile.sh` (GCC 7.2, profil automatique, rendu filtré,
1800 trames avec des appuis scriptés) :

| Console | Au plafond | Autres |
| --- | --- | --- |
| META (48 MHz) | 67 sur 68 à 98,8 % | H.E.R.O. 98,0 % |
| Pokitto (48 MHz) | 67 sur 68 à 99,0 % | H.E.R.O. 98,3 % |

Le plafond n'est pas 100 % : la mesure compte le démarrage du firmware, avant
la première trame SG-1000. H.E.R.O. ne perd que pendant ses 150 premières
images, puis tient la vitesse NTSC. Avant l'assembleur et ces profils (même
jour), la moyenne META était de 96,9 %, Space Slalom à 78 % ; avant la
recompilation, l'émulation du Z80 atteignait environ 32 % (Flicky, Wonder
Boy).

Le profilage de `compile.sh` joue une partie bouton 1 maintenu, une partie
sans touche (logos, titre, démo) et huit parties aléatoires. L'interpréteur y
note aussi les cibles des `RET`, `RETI`/`RETN` et `JP (HL)` (hors retours
d'interruption) en lignes `# entry XXXX` : le recompilateur en fait des
entrées de région (adresses de retour empilées, tables de sauts), sans quoi
ce code repasserait par l'interpréteur. `AUTO_REDO=1` refait les profils.

En retard, le firmware saute jusqu'à deux présentations de suite ; le Z80, le
PSG et l'état des sprites restent exacts. Clang 23 produit des
firmwares un peu plus petits et de 1 à 5 % plus lents que GCC 7.2 sur ces jeux.

## Limites

- Cartouches sans mapper, au plus 48 Kio ; RAM de cartouche limitée au cas
  8 Kio en `8000–BFFF`. Pas de SC-3000 (clavier), ni d’extension.
- Baseline NTSC : 262 lignes de 228 T-states. PAL n’est pas pris en charge.
- TMS9918A : rendu par ligne, pas au pixel près au moment du bus VRAM ; les
  combinaisons de modes non documentées ne sont pas validées.
- SN76489 : trois tons, bruit TI, atténuation 2 dB, sortie 8 bits à environ
  22 kHz, sans filtre analogique ni suréchantillonnage.
- Effets non documentés dépendant de MEMPTR/Q non modélisés ; pas de wait
  states de bus.

Les spécifications utilisées sont le
[manuel Zilog](https://www.zilog.com/docs/z80/z80cpu_um.pdf), les observations
[Z80 de Sean Young](https://www.smspower.org/uploads/Development/z80-documented.pdf),
le [manuel TI TMS9918A](https://www.smspower.org/uploads/Development/TMS9918.pdf)
et la [carte des ports SG-1000](https://www.smspower.org/Development/IOPortMapSG).

## Jouer sur PC

Le frontal commun `gm0`, sur sa [branche `z80`](https://github.com/r043v/gm0/tree/z80), charge directement les cartouches `.sg` dans le noyau
Z80/TMS9918A/PSG, sans seconde émulation Cortex-M0 sur PC. Il partage les
filtres F8, l’échelle F10, le plein écran F11, les manettes, F5, le titre et le
build WebAssembly avec META/Pokitto. Le petit lecteur `sg1000_play` reste un
harness SDL de référence pour les tests du noyau :

```bash
../gm0/build/bin/gm0 jeu.sg
```

Flèches : déplacement ; Espace : bouton 1 (A), Ctrl : bouton 2 (B).
Entrée : Start (bouton 1, utilisé pour démarrer Galaga) ; `*` : Select (Pause SG-1000,
également avec le `*` du pavé numérique) ;
Échap : quitter en natif. L’image est redimensionnable et le PSG produit ses
échantillons à 22050 Hz, rééchantillonnés par la sortie commune à 48000 Hz.
La cadence suit les 59736 T-states d’une trame NTSC à 3,579545 MHz.
`EMU_NOPACE=1` permet un essai sans attente ; `--frames N`, `--shot`, `--wav`
et `EMU_INPUT` sont disponibles pour les vérifications automatiques.

Le frontend des firmwares évite les divisions logicielles pour redimensionner
l’image, ne calcule le fond que pour les lignes réellement affichées, et saute
au plus une présentation consécutive lorsqu’il est en retard. Le Z80, le PSG
et les collisions de sprites continuent ; ce saut d’affichage ne garantit pas
la pleine vitesse du jeu sur une console physique. META utilise deux tampons
de lignes et le DMA LCD pour recouvrir le calcul et le transfert.

Le titre indique « actuel » (cadence émulée / temps mural, attente et affichage
inclus) et « possible » (cadence émulée / temps passé dans le noyau et le son,
hors présentation et synchronisation). 100 % représente la vitesse NTSC normale ;
la capacité possible n’est pas plafonnée, en natif comme dans le HUD WebAssembly.

Les distributions PC sont générées par `tools/sg1000/package.py` avec CMake et
Ninja, avec les sources de la branche `z80` de gm0 (`git switch z80` dans
le dépôt gm0). La branche `main` de gm0 exécute les firmwares META/Pokitto ;
la lecture directe des `.sg` est réservée à `z80`. Elles contiennent le lecteur, SDL2, les licences, une notice et un
manifeste SHA-256, sans cartouche. Linux fournit `sg1000-play` et `play.sh` ;
Windows fournit `sg1000_play.exe` et `SDL2.dll`. Sans argument, le lecteur
attend une cartouche déposée dans sa fenêtre.

```bash
python3 tools/sg1000/package.py \
  --frontend-root /chemin/gm0 \
  --sdl-linux-prefix /chemin/SDL2-linux \
  --sdl-mingw-root /chemin/SDL2/x86_64-w64-mingw32 \
  --mingw-root /chemin/llvm-mingw \
  --zlib-linux-prefix /chemin/zlib-linux \
  --zlib-mingw-prefix /chemin/zlib-mingw \
  --zlib-license /chemin/zlib/LICENSE \
  --linux-sysroot /chemin/sysroot-ubuntu22
```

Le sysroot Linux est facultatif. SDL2 doit aussi être construit contre la libc
choisie ; le manifeste et la notice indiquent la version minimale requise.
Les archives sont placées dans `output/sg1000-release/`.
L’option `--rom /chemin/jeu.sg --name galaga` intègre la cartouche fournie dans
les exécutables, sans l’ajouter aux sources suivies. Dans ce mode, le `.exe`
Windows inclut aussi SDL2 statiquement et démarre le jeu sans argument ni DLL
SDL2 externe. La distribution générique reste dépourvue de cartouche.
Le build web est celui de `gm0` : `emcmake cmake -G Ninja -S /chemin/gm0
-B build-wasm -DGM0_WEB_GAME=/chemin/Galaga.sg`, puis `cmake --build build-wasm`.
Il produit un seul `gm0.html` contenant le module et le jeu, sans réseau requis.

`./compile.sh` détecte par défaut les jeux de `roms/gb/` et `roms/sg/`. Un dossier
explicite accepte également des `.gb`, `.gbc` et `.sg`, ou les sous-dossiers
`gb/` et `sg/`. `TARGET=both ./compile.sh roms/sg` construit pour les deux
consoles. Les projets SG sont dans `out/sg1000/<cible>/<jeu>/`, les couvertures
sont conservées dans `<source>/profiles/<sha1>/sg1000/` et les jeux pour la carte
gardent le même inventaire, bilan et contrôle de version que les jeux GB.
Le profil SG automatique joue `AUTO_SECONDS` secondes émulées en tenant le
bouton 1 ; `SG_AUTO_FRAMES` permet de régler ce nombre d’images. `PLAY=1` ouvre
le lecteur natif. Le frontend firmware SG utilise GCC ; les options PACK,
les états et les sauvegardes propres à la Game Boy ne lui sont pas appliqués.
