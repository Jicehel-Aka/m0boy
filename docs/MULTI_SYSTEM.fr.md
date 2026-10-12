# CPU, console d’origine et destination

La sélection d’un jeu comporte désormais trois identités indépendantes :

| Axe | Game Boy | SG-1000 |
| --- | --- | --- |
| Console d’origine (`--system`) | `gameboy` | `sg1000` |
| CPU requis (`--cpu`, contrôle facultatif) | `sm83` | `z80` |
| Destination (`--target`) | `desktop`, `meta`, `pokitto` | Projets natifs expérimentaux `desktop`, `meta`, `pokitto` |
| État actuel | Pipeline existant | Exécution Z80, TMS9918A et génération native des cartouches sans mapper |

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
- `recompiler/src/system/sg1000/`, `include/m0boy/sg1000.h` : cartouche SG-1000.
- `m0boy_frontends` : chargement et métadonnées des consoles d’origine.
- `gbrecomp_core` : analyse, IR et générateurs Game Boy existants.
- `targets/meta/` : runtime Game Boy et pilotes des destinations existantes.
- `targets/sg1000/` : noyau Z80 C, bus SG-1000, TMS9918A et frontend natif.
  La génération réutilise les pilotes écran, horloge, démarrage et boutons de
  META/Pokitto, avec une configuration matérielle minimale.

Les anciens includes `recompiler/rom.h` et `recompiler/decoder.h`, le namespace
`gbrecomp` et l’exécutable `gbrecomp` restent utilisables. Le nouvel exécutable
`m0recomp` utilise exactement la même CLI. `compile.sh` et le frontend Node
continuent à utiliser le pipeline Game Boy.

L’IR, les optimisations et les générateurs Game Boy gardent leur sémantique
SM83/Game Boy. Ils ne constituent pas encore un IR commun au Z80. Le runtime
`gbl` reste propre à la Game Boy ; aucun périphérique SG-1000 n’y est ajouté.

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

Une cartouche SG-1000 n’a pas besoin d’un en-tête Nintendo. Le premier profil
accepte de 1 à 48 Kio de ROM brute, sans mapper ni RAM de cartouche. Les tailles
supérieures sont refusées avec un diagnostic explicite. Le lecteur statique
expose les octets du fichier à partir de l’adresse zéro ; il retourne `0xff`
hors du fichier et pour toute banque autre que zéro. Ce lecteur ne simule
pas les miroirs électriques de ROM ou la RAM de la console.

Le profil déclare le reset Z80 à `0000` et 1 Kio de RAM système. La
[carte mémoire SG-1000](https://www.smspower.org/Development/MemoryMap)
place la cartouche dans `0000–BFFF` et la RAM dans `C000–C3FF`, avec des
miroirs jusqu’à `FFFF`. Les extensions et cartouches qui remplacent cette
carte mémoire demanderont leurs propres profils.

Le désassemblage est un balayage linéaire : des données peuvent apparaître
comme des instructions. `--limit` limite le nombre d’instructions affichées.
L’analyse de contrôle de flux Z80 indépendante (`--analyze`) n’est pas encore
exposée. La génération suit les branches statiques ou les adresses exécutées
d’une couverture SG-1000 v1. Elle accepte au maximum 1024 entrées natives par
projet ; les autres instructions et tout code en RAM passent par l’interpréteur.
`--meta-code-budget` peut réduire ce nombre, avec une estimation conservatrice
à 160 octets par entrée ; le rapport du linker reste la vérification réelle.
Le harness classe les adresses par fréquence ; les entrées les plus chaudes
sont retenues en premier quand le budget est limité. Les couvertures Game Boy
sont refusées.

Les instructions natives spécialisent les opérations de `z80_ops.h` avec les
opcodes et opérandes constants du programme. Le compilateur C élimine le
décodage et les branches des autres instructions. L’interpréteur appelle ces
mêmes opérations avec des instructions décodées au moment de l’exécution.
Le code natif regroupe les instructions contiguës jusqu’au prochain événement
vidéo. Les accès aux ports et le délai EI restent exécutés instruction par
instruction, pour conserver les interruptions et les écritures PSG dans le temps.
Les instructions de bloc restent interruptibles entre leurs répétitions.

Le harness généré se construit avec CMake/Ninja dans `host/` et accepte
`--interp`, `--frames`, `--coverage`, `--buttons`, `--start-frame`,
`--pause-frame`, `--shot` et `--wav` (PCM mono 8 bits, 22050 Hz). Les bits de `--buttons` sont Haut, Bas, Gauche,
Droite, bouton 1 et bouton 2 (bits 0 à 5). `--start-frame` tient le bouton 1
pendant dix images. Le `run_hash` compare les registres, cycles, mémoire,
état VDP/PSG et pixels ; il exclut les compteurs de répartition natif/interprété.

Chaque projet possède un seul système de destination. Utiliser un répertoire
séparé pour META et Pokitto. Pour META, la génération produit `platformio.ini` :
`pio run -d output/jeu-meta`. Pour Pokitto, le firmware est produit par le
CMake/Ninja ci-dessus, avec le checksum de vecteurs LPC vérifié après liaison.
La sortie d’image conserve le rapport 4:3 : 160×120 au centre du META et
200×150 au centre du Pokitto. MENU/C fournit une impulsion NMI à l’adresse 66.
Le son et le menu de configuration Game Boy ne sont pas repris dans ce premier
frontend SG-1000.

Le SM83 est apparenté au Z80, mais n’est pas un Z80 interchangeable. Les
codes de registres et certains encodages sont communs ; les décodeurs les
partagent sans partager les drapeaux ni les timings. Les préfixes indexés,
les registres alternatifs et les ports d’E/S restent propres au Z80 ; les
instructions `LDH`, `STOP` et `SWAP` restent propres au SM83. Le décodeur Z80
marque les encodages non documentés, dont `SLL` et les accès `IXH/IYH`. Il
fournit des métadonnées de désassemblage ; le noyau C fournit les opérations
exécutables partagées par les chemins natif et interprété.

## État et limites de cette première exécution

- Cartouches sans mapper, au plus 48 Kio ; miroirs de ROM pour les tailles
  puissance de deux, octets absents à `FF` pour les tailles irrégulières.
- Noyau Z80 : instructions principales et préfixes `CB/ED/DD/FD`, registres
  alternatifs et indexés, interruption IM1/IM2, IM0 limité aux instructions
  RST, NMI, inhibition EI et rafraîchissement R. Pas de wait states de bus.
- TMS9918A : modes graphiques I/II, texte et multicolore, sprites, priorité,
  collision, cinquième sprite, ports à latch/read-ahead et IRQ de VBlank.
  Le rendu est effectué par ligne, pas par pixel au moment du bus VRAM.
  Les combinaisons de modes non documentées ne sont pas validées.
- Baseline NTSC : 262 lignes de 228 T-states. PAL n’est pas pris en charge.
- Synthèse SN76489 : trois tons, bruit TI, atténuation 2 dB, écritures
  horodatées dans la trame ; DAC META/Pokitto et capture WAV hôte.
  Sortie 8 bits à environ 22 kHz, sans filtre analogique ni suréchantillonnage.
- Les effets Z80 non documentés dépendant de MEMPTR/Q ne sont pas validés.
  Aucun résultat ZEXALL ou mesure de vitesse sur une console physique.

Les tests utilisent des programmes synthétiques : référence exhaustive de
l’arithmétique 8 bits, BCD, prefixes, cycles, interruptions, bus et ports VDP,
comparaisons des pixels et `run_hash` entre chemins natif/interprété, repli
sur un budget natif presque vide, et liaison Cortex-M0+ Pokitto quand la
chaîne ARM est disponible. Le programme utilisateur Galaga est conservé dans
les sorties locales ignorées : après 300 images, les chemins natif/interprété
ont le même hash et la même capture. Les firmwares META et Pokitto sont liés
et essayés dans gm0, qui conserve son architecture META/Pokitto.

La suite prioritaire est la validation Z80 avec des exercisers indépendants,
la lecture d’entrées interactive sur le harness hôte,
et l’optimisation des boucles avant d’affirmer une compatibilité ou une vitesse
sur matériel réel. Les spécifications utilisées sont le
[manuel Zilog](https://www.zilog.com/docs/z80/z80cpu_um.pdf), les observations
[Z80 de Sean Young](https://www.smspower.org/uploads/Development/z80-documented.pdf),
le [manuel TI TMS9918A](https://www.smspower.org/uploads/Development/TMS9918.pdf)
et la [carte des ports SG-1000](https://www.smspower.org/Development/IOPortMapSG).

## Jouer sur PC

Avec SDL2 installé, CMake construit aussi `build/bin/sg1000_play`. Le lecteur
natif utilise le même noyau Z80/TMS9918A/PSG que les firmwares et évite le coût
d’une seconde émulation Cortex-M0 sur PC :

```bash
build/bin/sg1000_play jeu.sg
```

Flèches : déplacement ; Espace : bouton 1 (A), Ctrl : bouton 2 (B).
Entrée : Start (bouton 1, utilisé pour démarrer Galaga) ; `*` : Select (Pause SG-1000,
également avec le `*` du pavé numérique) ;
Échap : quitter. L’image est redimensionnable et le son mono tourne à 22050 Hz.
La cadence suit les 59736 T-states d’une trame NTSC à 3,579545 MHz.
`--frames N` permet un essai SDL sans attente ; `--start-frame N` et `--shot`
sont disponibles pour les vérifications automatiques.

Le frontend des firmwares évite les divisions logicielles pour redimensionner
l’image, ne calcule le fond que pour les lignes réellement affichées, et saute
au plus une présentation consécutive lorsqu’il est en retard. Le Z80, le PSG
et les collisions de sprites continuent ; ce saut d’affichage ne garantit pas
la pleine vitesse du jeu sur une console physique. META utilise deux tampons
de lignes et le DMA LCD pour recouvrir le calcul et le transfert.

Le titre indique « actuel » (cadence émulée / temps mural, attente et affichage
inclus) et « possible » (cadence émulée / temps passé dans le noyau et le son,
hors présentation et synchronisation). 100 % représente la vitesse NTSC normale ;
la capacité possible n’est pas plafonnée. `--frames N --realtime` conserve
l’attente de synchronisation pendant un essai borné.

Les distributions PC sont générées par `tools/sg1000/package.py` avec CMake et
Ninja. Elles contiennent le lecteur, SDL2, les licences, une notice et un
manifeste SHA-256, sans cartouche. Linux fournit `sg1000-play` et `play.sh` ;
Windows fournit `sg1000_play.exe` et `SDL2.dll`. Sans argument, le lecteur
attend une cartouche déposée dans sa fenêtre.

```bash
python3 tools/sg1000/package.py \
  --sdl-linux-prefix /chemin/SDL2-linux \
  --sdl-mingw-root /chemin/SDL2/x86_64-w64-mingw32 \
  --mingw-root /chemin/llvm-mingw \
  --linux-sysroot /chemin/sysroot-ubuntu22
```

Le sysroot Linux est facultatif. SDL2 doit aussi être construit contre la libc
choisie ; le manifeste et la notice indiquent la version minimale requise.
Les archives sont placées dans `output/sg1000-release/`.
L’option `--rom /chemin/jeu.sg --name galaga` intègre la cartouche fournie dans
les exécutables, sans l’ajouter aux sources suivies. Dans ce mode, le `.exe`
Windows inclut aussi SDL2 statiquement et démarre le jeu sans argument ni DLL
SDL2 externe. La distribution générique reste dépourvue de cartouche.

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
