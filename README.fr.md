# m0boy

**Les jeux Game Boy, SG-1000, Game Gear et Master System recompilés en
firmwares natifs pour deux consoles Cortex-M0+ : la Gamebuino META et la
Pokitto.**

*English version: [README.md](README.md).*

m0boy transforme une ROM Game Boy (DMG) en un dossier à copier sur la carte
SD de la console. Le code du jeu est traduit en C puis compilé pour le
processeur de la console : ce n'est ni un émulateur ni un JIT. Le code vu
pendant des parties automatiques est recompilé, et un petit interpréteur
exécute le reste. Le PPU, l'APU, les timers et les mappers MBC sont
réimplémentés dans un runtime compact dont les parties chaudes sont en
assembleur Thumb-1.

Le projet est parti d'un portage de [gb-recompiled](https://github.com/arcanite24/gb-recompiled)
d'arcanite24, un recompilateur qui vise les ordinateurs de bureau. m0boy
garde son recompilateur et ajoute tout ce qu'il faut pour tourner sur des
puces à 32 Kio de RAM, 256 Kio de flash et sans division matérielle.

## Consoles

| | Gamebuino META | Pokitto |
|---|---|---|
| Puce | SAMD21, Cortex-M0+ à 48 MHz (72 MHz en option, hors spécification) | LPC11U68, Cortex-M0+ à 48 MHz (72 MHz en option) |
| Mémoire | 32 Kio de RAM, 240 Kio de flash pour le firmware | 32 + 2 + 2 Kio de RAM, 228 Kio de flash sous le loader |
| Écran | 160×128 : les 144 lignes du Game Boy fusionnées en 128 | 220×176 : image 1:1, ou agrandie en 200×176 |
| Build du firmware | PlatformIO | `make` avec arm-none-eabi-gcc |
| Version | 0.10.27 | 0.10.21 |
| État | Jeux lancés sur la console | Testée en émulateur seulement. Un premier testeur sur vraie Pokitto signale une image retournée verticalement (lignes inversées, colonnes dans l'ordre), compatible avec la perte du bit SS (R01) du panneau à l'init ; la 0.10.1 allonge l'attente après reset et l'impulsion d'écriture des registres et réécrit R01, à vérifier sur la console |

## SG-1000

Les cartouches Sega SG-1000 (`.sg`, sans mapper, 48 Kio au plus) suivent la
même chaîne : le code Z80 est recompilé, pas émulé.

- **Recompilateur** (`--system sg1000`) : le code joué pendant les parties
  automatiques (bouton 1 maintenu, aucune touche pour les logos et les
  démos, parties aléatoires), puis celui qu'il atteint statiquement,
  devient des régions de C comme avec l'émetteur Game Boy : `goto`
  internes, `CALL` en appel C, drapeaux calculés seulement quand une
  instruction suivante les lit (cas courants en ligne), cycles ajoutés par
  lots, synchronisation aux têtes de boucle. Les cibles de `RET` et de
  `JP (HL)` vues au profilage (adresses de retour empilées, tables de sauts)
  deviennent des entrées de région. Les instructions rares reprennent la
  sémantique de l'interpréteur Z80 sur opérandes constants ; l'interpréteur
  exécute ce qui n'a jamais été vu.
- **Runtime** : deux événements par trame au lieu d'un test par
  instruction. Les lignes TMS9918A sont dessinées quand le programme touche
  le VDP ou au VBlank, réduites de 256×192 par un filtre boîte exact (poids
  8 → 5 en table : deux couleurs par tuile ; boucles de tuiles en
  assembleur Thumb-1, exécutées en RAM). Les masques de sprites par ligne
  suivent chaque écriture d'un Y de sprite. Le SN76489 est rattrapé avant
  chaque écriture, par blocs partagés entre ses quatre canaux. La RAM de
  cartouche de 8 Kio (The Castle) est détectée au profilage. `OUTI`/`OUTD`,
  que les jeux déroulent pour remplir la VRAM, sont compilés comme sur Game
  Gear au lieu d'appeler l'interpréteur.
- **Vérifié** : avec `--meta-precise`, le code recompilé reproduit
  exactement l'interpréteur (registres, mémoire, VDP, PSG, image) sur les
  68 cartouches d'une collection. Le noyau Z80 passe ZEXDOC et 66 tests
  ZEXALL sur 67.
- **Vitesse**, mesurée dans l'émulateur gm0 (pas sur console), à 48 MHz :
  67 de ces 68 jeux tournent à pleine vitesse NTSC sur META comme sur
  Pokitto (H.E.R.O. ralentit seulement au démarrage) ; environ 32 % avec
  l'ancienne émulation.

## Game Gear

Les cartouches Game Gear (`.gg`) passent par le même recompilateur Z80,
étendu au code en banques :

- **Recompilateur** (`--system gg`) : une instruction est une page et une
  adresse ; une région couvre une fenêtre de 16 Kio d'une page. Un appel
  vers une autre fenêtre vérifie sa page avant d'y sauter directement, sinon
  le répartiteur trouve la région de la bonne page ; le code rend la main
  dès que le mapper déplace la page d'où il s'exécute. Mappers Sega et
  Codemasters.
- **Runtime** : VDP mode 4 (tuiles, défilement et verrouillages, sprites,
  priorité, interruptions de ligne, CRAM 12 bits) dessiné 1:1 dans la
  fenêtre de 160×144, tuiles de fond en assembleur Thumb-1 ; PSG Sega (mono).
- **Grosses ROM et sauvegardes** : une ROM qui ne tient pas en flash avec
  son code y garde la page 0, les instructions qu'elle compile et les
  secteurs de ROM de 512 octets qui, d'après les parties enregistrées
  (exécutions et lectures de secteur par image), font gagner le plus de
  cycles par octet ; elle lit les autres dans une copie sur la carte SD. La
  RAM de cartouche des jeux à sauvegarde vit dans un fichier `.SAV` sur la
  carte. Les deux partagent un petit cache de secteurs (quatre sur la Game Gear Pokitto,
  deux ailleurs) : les 24 Kio de RAM vidéo et de travail de la console
  d'origine laissent peu de place. `compile.sh` enregistre les parties d'un
  jeu Game Gear ou Master System : des parties à l'aveugle (10 800 images
  chacune) et quatre explorations (`--explore`, 120 000 images chacune)
  où le joueur garde l'état de toutes les parties où il a vu du code ou des
  données nouveaux et y repart, pour atteindre des niveaux qu'un joueur à
  l'aveugle ne voit pas. L'explorateur (`targets/common/host/m0_explore.h`)
  est commun aux harnais Game Boy, SG-1000 et Game Gear/Master System
  (`EXPLORERS`, `EXPLORE_FRAMES`) ; sur Game Boy il repart d'états
  sauvegardés, 400 000 images par défaut. Les anciennes parties, sans les comptes d'exécutions
  ou sans exploration, sont refaites. Les secteurs dont l'interpréteur lit le
  code, pour ce qui n'est pas compilé, comptent comme ceux lus en données.
- **Vérifié** : avec `--meta-precise`, le code recompilé reproduit
  exactement l'interpréteur sur les 225 cartouches d'une collection Game Gear.
- **Vitesse**, mesurée dans gm0 à 48 MHz sur les jeux essayés : pleine
  vitesse sur les deux consoles (Columns, Pac-Man, GG Aleste, Fantasy Zone ;
  Aladdin, lu en grande partie sur la carte, 94 % sur META et 98 % sur
  Pokitto).

## Master System

Les cartouches Master System (`.sms`, `--system sms`) partagent le
recompilateur et le runtime de la Game Gear. Toute l'image de 256×192 est
montrée, réduite de 8 à 5 en largeur comme le fait la Game Gear en mode
SMS : deux pixels voisins de 4 bits indexent une table de mélanges par
palette, la boucle du fond en Thumb-1. Une ligne sur 8/5 est dessinée
(160×120 sur META, 200×150 sur Pokitto). PAUSE est MENU / C. Alex Kidd in
Miracle World, Shinobi et Wonder Boy III tournent à 97–99 % de la vitesse sur
les deux consoles (gm0). Pas encore de son FM.

Sur PC, la lecture passe par la [branche `z80` de gm0](https://github.com/r043v/gm0/tree/z80) :
`gm0 jeu.sg`. Détails : [docs/MULTI_SYSTEM.fr.md](docs/MULTI_SYSTEM.fr.md).

## Le travail réalisé

### Une recompilation guidée par le jeu

- Avant de compiler une ROM, `compile.sh` y joue tout seul : une partie
  à entrées aléatoires par cœur du processeur, 20 s chacune à pleine
  vitesse (3 000 à 12 000 images par seconde, donc 15 à 60 minutes de jeu
  par partie), quatre explorations (plus bas) et les parties que tu
  enregistres toi-même. Seules les
  instructions exécutées pendant ces parties sont compilées : les données
  ne sont jamais prises pour du code, et le firmware tient en flash.
- Tout le reste, y compris le code copié en RAM, passe par l'interpréteur
  `gbl`. Code compilé et interpréteur se relaient à n'importe quelle
  instruction.
- Le générateur reconnaît les boucles d'attente (lecture de LY, de STAT,
  du joypad ou d'un drapeau posé par une interruption, boucles de délai)
  et saute des itérations entières jusqu'au prochain événement. Registres,
  drapeaux et compte de cycles restent identiques à une exécution complète.
- Les drapeaux ne sont calculés que là où une instruction suivante peut
  les lire, et les cycles sont additionnés par portion de code linéaire.
- Un mode précis teste les interruptions avant chaque instruction. Ses
  builds donnent exactement les mêmes images que l'interpréteur (Super Mario
  Land, 3 000 images) et servent aux tests différentiels.

### Toutes les tailles de ROM dans 240 Kio de flash

- **Les petites ROMs** tiennent en flash à côté du runtime (environ 38 Kio)
  et du code compilé.
- **ROMs compressées** (`PACK=1`, par défaut) : la banque 0 et les blocs de
  4 Kio les plus lus restent en clair et sont lus directement ; chaque autre
  secteur de 512 octets est compressé en LZSS dans la flash et décompressé
  à la demande dans un petit cache, par un décodeur Thumb-1 qui tourne en
  RAM. La ROM tombe à 63–78 % de sa taille, et la flash libérée va au code
  compilé : sur R.C. Pro-Am, 68 000 instructions interprétées au lieu de
  188 000 sur 400 images, 6 % de cycles en moins par image (estimation).
- **Les ROMs de 256 Kio et plus** sont lues sur la carte SD au fil du jeu.
  La banque 0 et les blocs les plus lus restent en flash, choisis d'après
  les profils de lecture enregistrés pendant les parties ; le reste est lu
  par secteurs de 512 octets dans un cache LRU de 8 secteurs. Le firmware
  trouve le fichier de la ROM en FAT16/FAT32 au démarrage. Il n'y a pas de
  limite de taille : Link's Awakening, Pokémon Jaune et Donkey Kong Land
  sont convertis ainsi.
- **La RAM de cartouche** de plus de 2 Kio (8 Kio pour Link's Awakening,
  32 Kio pour Pokémon) vit dans un fichier `.SAV` sur la carte, derrière un
  cache en écriture différée de deux secteurs : ces jeux tiennent ainsi dans
  32 Kio de RAM. Les sauvegardes sont écrites quand le jeu désactive sa RAM,
  deux secondes après la dernière modification, et avant le retour au
  loader.

### La vitesse sur un Cortex-M0+ à 48 MHz

- Les parties chaudes sont en assembleur Thumb-1 écrit pour le M0+ : la
  machine à états des lignes du PPU et le rendu des lignes, les accès
  mémoire et registres, l'interpréteur, le cache de dispatch du code
  compilé, l'OAM DMA, l'envoi des pixels de la Pokitto et le décodeur LZSS.
  Sur 15 jeux, le temps d'émulation par image a baissé de 15 % à 53 %
  (médiane 19 %), avec des images identiques au runtime en C.
- Les périphériques sont paresseux : un seul compteur de cycles pilote
  tout, le PPU dessine une ligne entière à la fin du mode 3, et les timers,
  STAT, LYC et VBlank ne lèvent un événement que lorsqu'il devient
  observable.
- Le M0+ n'a pas de division matérielle : les divisions ont été retirées
  des chemins chauds.
- Faute de console pour tout mesurer, m0boy a son propre estimateur de
  cycles Cortex-M0+ (`targets/meta/tools/m0bench`). **Les chiffres de
  performance de ce dépôt sont des estimations de ce modèle, pas des
  mesures sur console.**

### Image, son et menu

- META : TBLEND, le mode d'affichage par défaut, fusionne la dernière ligne
  de chaque rangée de tuiles avec la suivante, donc pas de texte coupé, pour
  environ 2 % du temps d'une image. D'autres modes échangent qualité et
  vitesse. Pokitto : l'image en 1:1 au centre de l'écran, ou agrandie en
  200×176. Les lignes inchangées ne sont pas renvoyées.
- Son : l'APU synthétise une image entière d'échantillons au VBlank. Sur la
  META il passe par le DAC en DMA ; sur la Pokitto par son DAC R2R, depuis
  une interruption en assembleur.
- Palettes : vert par défaut. Les palettes du boot ROM Game Boy Color sont
  incluses : maintenir une direction (+ A ou B) au démarrage, comme sur une
  GBC ; la palette GBC propre au jeu est proposée quand son titre est
  reconnu.
- Menu d'options (HOME maintenu sur la META, C sur la Pokitto) : redémarrer,
  10 emplacements de sauvegarde d'état, volume, mode d'affichage, palette,
  saut d'images, vitesse, 72 MHz, retour au loader.

### Deux consoles, un seul code

- Le runtime, le code généré et le frontend sont communs ; seuls les
  pilotes diffèrent. Chaque optimisation a sa valeur par console dans
  `targets/meta/runtime/gbl_profile.h`.
- Les firmwares sont bare metal : pas de cœur Arduino sur la META, ni mbed
  ni PokittoLib sur la Pokitto. Démarrage, horloges, SPI, écran, son,
  boutons et carte SD sont écrits pour chaque puce.
- Les cibles Sega (SG-1000, Game Gear, Master System) partagent plus que le
  cœur Z80 : un gabarit CMake par binaire, les helpers des harnais, le
  squelette de façade, l'interface du code généré (`sg_gen.h`) et le PSG
  (`sg_psg.h`) vivent dans `targets/common/` ; ce qui diffère est derrière un
  aiguillage (`sg_config.h`) ou une macro.
- Les débits du bus META sont dans `targets/meta/runtime/gbl_board.h` :
  écran à 24 MHz (le débit de meta-lapinou sur ce panneau ; 36 et 48 viennent
  de la PLL et s'essaient au menu « SPI ecran » du firmware Game Boy, ou par
  les `build_flags` d'un jeu, `-DMETA_LCD_SPI_MHZ=36`), carte SD à 24 MHz
  après l'initialisation à 250 kHz, boutons à 12 MHz.

### Les outils

- `compile.sh` convertit un dossier entier de ROMs pour une console ou les
  deux : parties automatiques, profils, build du firmware, icônes et écrans
  titres du loader, fichiers de sauvegarde, suivi des versions (un firmware
  fait par une ancienne version est proposé à la reconstruction).
- `tools/meta-compiler` : le même convertisseur en Node, avec une page web
  locale.
- `tools/gb-emu` : un fork de [gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
  d'Andy O'Neill qui fait tourner ces firmwares.
- [gm0](https://github.com/r043v/gm0) : l'émulateur META et Pokitto au
  cycle près, écrit en même temps que m0boy pour le tester, maintenant dans
  son propre dépôt.
- Tests : PPU comparé à une référence pixel par pixel, simulation de
  l'écran de la META, générateur de bruit de l'APU, recherche FAT, et builds
  complets pour les deux consoles.

Sur la collection de test de r043v (1 661 ROMs), la dernière conversion
(8 octobre 2026) a produit 996 firmwares META et 760 firmwares Pokitto. Ce
sont des builds réussis, pas des jeux vérifiés un par un.

## Démarrage rapide

Il faut CMake 3.20 ou plus, Ninja, gcc ou clang, Python 3, et
[PlatformIO](https://platformio.org/) pour la META. La Pokitto n'a besoin
que d'`arm-none-eabi-gcc` (celui du PATH, sinon celui de PlatformIO).

```bash
cmake -G Ninja -B build .
ninja -C build                 # build/bin/gbrecomp

# Mets tes ROMs dans roms/gb/ (Game Boy) et roms/sg/ (SG-1000), puis :
./compile.sh                   # demande la console (META par défaut)
TARGET=pokitto ./compile.sh    # ou directement la Pokitto
TARGET=both ./compile.sh       # ou les deux
```

`FIRMWARE_COMPILER=clang` compile les firmwares avec Clang (GCC par défaut).
Pour un nouveau build, CMake préfère Clang lorsqu'il est disponible,
sinon GCC. `-DM0BOY_PREFER_CLANG=OFF` privilégie GCC ; `CC`, `CXX` et une
toolchain explicite restent prioritaires. Un répertoire déjà configuré
conserve son compilateur. Voir [le guide des compilateurs](docs/COMPILATEURS.fr.md).

Chaque jeu arrive dans `roms/meta/<source>/<Jeu>/` ou
`roms/pokitto/<source>/<Jeu>/`, `<source>` étant la plateforme de la ROM
(`gb`, `sg`, `gg` ou `sms`). Copie ce dossier tel quel à la racine de la
carte SD, puis lance-le depuis le loader de la console. Le guide complet
est [docs/GUIDE.fr.md](docs/GUIDE.fr.md) ;
les notes techniques (en anglais) sont dans [targets/meta/README.md](targets/meta/README.md).

Tests :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

## Limites

- Jeux DMG seulement : les ROMs réservées à la Game Boy Color sont
  refusées et les fonctions CGB ne sont pas émulées.
- Mappers : aucun, MBC1, MBC2, MBC3 sans horloge, MBC5.
- Timing simplifié : le mode 3 dure toujours 172 points, les lignes sont
  dessinées d'un bloc, l'OAM DMA est instantané, le son est quantifié à
  l'image, le câble link n'a pas de partenaire.
- Sur la META, les 144 lignes sont ramenées à 128.
- Des images sont sautées quand la console prend du retard.
- Le code Game Boy jamais atteint pendant les parties enregistrées passe par
  l'interpréteur, bien plus lent : le château du game over de Super Mario
  Land 2 tourne à environ 37 images par seconde sur la META. Enregistrer une
  partie qui y passe avec `compile.sh`.
- La RAM de cartouche désactivée se lit `$FF`. Tokyo Disneyland y lit un
  pointeur et écrit à travers, ce qui tombe alors sur IE (`$FFFF`) ; non
  testé.
- SG-1000 : cartouches sans mapper de 48 Kio au plus, NTSC seulement, sans
  clavier SC-3000 ; le code jamais vu au profilage passe par l'interpréteur.

## 100 % vibe coded

Aucune ligne du portage (tout ce que m0boy ajoute à gb-recompiled) n'a été
écrite à la main. Le code, les tests, les pilotes bare metal et cette
documentation ont été écrits par des agents d'IA, guidés par r043v, qui a
testé sur les consoles et fait les choix.

**Claude** (Claude Opus 5.5 dans Claude Code) a écrit l'essentiel. Ses
chiffres sur ce projet, du 29 septembre au 9 octobre 2026 :

| | |
|---|---|
| Sessions Claude Code | 10 |
| Messages de r043v | 135 |
| Réponses du modèle | 3 329 |
| Appels d'outils | 3 303, dont 3 072 commandes shell |
| Tokens générés | 3,54 millions |
| Tokens de contexte relus en cache | 1,37 milliard |
| Commits | 67 |
| Lignes ajoutées par ses commits | 42 375 (95 %) |

Un second agent (GLM, via ZCode) a écrit le reste : 26 commits,
2 207 lignes (5 %), dont le convertisseur web
(`tools/meta-compiler/web.mjs`), les palettes du boot ROM Game Boy Color
(runtime 0.7.0) et des corrections Pokitto. Il a aussi écrit l'émulateur
[gm0](https://github.com/r043v/gm0).

Un troisième agent (DeepSeek V4.1 Flash Free, via OpenCode) s'est occupé des
bus et du code dupliqué : carte SD passée de 12 à 24 MHz, un débit par
périphérique du SERCOM4, et les cibles SG-1000, Game Gear et Master System
réunies sur un seul gabarit CMake, un seul jeu de helpers, un seul squelette
de façade et une seule interface de code généré. 9 commits, 39 fichiers,
861 lignes ajoutées et 859 supprimées — le même code à moins d'endroits. Il a
aussi monté l'écran à 48 MHz puis ramené à 24 en 0.10.9 : rien au-dessus de 24
n'est passé sur une vraie META, et 36/48 restent à une touche du menu.

Les chiffres viennent des transcriptions de Claude Code et de l'historique
git. Le recompilateur de départ (`recompiler/`, environ 17 000 lignes) est
celui de gb-recompiled.

### Historique

Ce dépôt garde l'historique de gb-recompiled (104 commits
d'arcanite24 et de ses contributeurs), puis celui du portage. En ont été
retirés :

- l'émulateur (`tools/meta-emu-sdl`), devenu [gm0](https://github.com/r043v/gm0) :
  les 61 commits qui ne touchaient que lui ont disparu ;
- les premiers dossiers de test de gb-recompiled qui contenaient des ROMs
  commerciales converties en C (`pokemon_test`, `test_output_tetris`,
  `tetris_test`, `tetris_test_analysis`, `tetris_fixed`), et une capture
  audio de débogage (`debug_audio.raw`, `debug_audio.wav`).

Les parties de gb-recompiled propres aux ordinateurs de bureau (runtime
SDL, ports, outils de précision) ont ensuite été retirées de l'arbre ; elles
restent dans l'historique et dans le [dépôt d'origine](https://github.com/arcanite24/gb-recompiled).

## Organisation du dépôt

| Chemin | Contenu |
|---|---|
| `recompiler/` | `gbrecomp`/`m0recomp`, avec la cible META et Pokitto (`--target meta`, `--target pokitto`) et le recompilateur Z80 SG-1000 (`src/system/sg1000/`) |
| `targets/meta/runtime/` | le runtime `gbl` commun aux deux consoles |
| `targets/meta/platform/` | frontend commun (menu, sauvegardes, carte SD, affichage) et pilotes de chaque console |
| `targets/meta/host/`, `targets/meta/tools/` | harnais PC, estimateur de cycles, outils de police |
| `targets/sg1000/` | runtime SG-1000 (TMS9918A, SN76489, interpréteur Z80 de repli), frontend des consoles, harnais PC |
| `targets/gamegear/` | runtime Game Gear/Master System (VDP mode 4, PSG Sega, pagination, sauvegardes), frontend des consoles, harnais PC |
| `targets/common/` | ce que les cibles Sega partagent : gabarits CMake, helpers des harnais, explorateur de jeu, squelette de façade, `sg_gen.h`, `sg_psg.h` |
| `compile.sh`, `tools/meta-compiler/` | conversion en lot |
| `tools/gb-emu/` | fork de gamebuino-emulator |
| `tests/`, `targets/meta/tests/` | tests |
| `docs/` | guide utilisateur, état du développement |

## ROMs et droits

Aucune ROM et aucun firmware compilé ne sont fournis. Convertis seulement
des jeux que tu possèdes, ou des homebrews dont la licence le permet.

## Licence

MIT, voir [LICENSE](LICENSE). Basé sur [gb-recompiled](https://github.com/arcanite24/gb-recompiled),
© 2026 arcanite24, licence MIT. `tools/gb-emu` garde sa propre licence MIT
(© 2017 Andy O'Neill).
