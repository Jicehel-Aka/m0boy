# m0boy

**Les jeux Game Boy recompilés en firmwares natifs pour les consoles
Cortex-M0+ : Gamebuino META et Pokitto.**

> *English:* m0boy statically recompiles Game Boy (DMG) ROMs to C and builds
> them into native firmwares for the Gamebuino META (SAMD21) and the Pokitto
> (LPC11U68). It is a port of [gb-recompiled](https://github.com/arcanite24/gb-recompiled)
> by arcanite24, extended with a small runtime written for 48 MHz Cortex-M0+
> chips with 32 KiB of RAM. No ROM is included. The documentation is in French
> ([META.md](META.md)).

Une ROM `.gb` devient un dossier à copier sur la carte SD de la console.
Le code du jeu est traduit en C puis compilé pour le processeur de la
console : ce n'est ni un émulateur ni un JIT. Le code repéré pendant des
parties de profilage automatiques est recompilé, et un petit interpréteur
exécute le reste. Le PPU, l'APU, les timers et les mappers MBC sont
réimplémentés dans un runtime compact (`targets/meta/runtime`), dont les
parties chaudes sont en assembleur Thumb-1.

## 100 % vibe coded

Aucune ligne du portage (tout ce que m0boy ajoute à gb-recompiled) n'a
été écrite à la main. Le code, les tests, les pilotes bare metal et cette
documentation ont été écrits par des agents d'IA, guidés par r043v, qui a
testé sur les consoles et fait les choix.

**Claude** (Claude Opus 5.5 dans Claude Code) a écrit l'essentiel. Ses
chiffres sur ce projet, du 29 septembre au 9 octobre 2026 :

| | |
|---|---|
| Sessions Claude Code | 10 |
| Messages de r043v | 134 |
| Réponses du modèle | 3 285 |
| Appels d'outils | 3 259, dont 3 031 commandes shell |
| Tokens générés | 3,50 millions |
| Tokens de contexte relus en cache | 1,36 milliard |
| Commits | 66 |
| Lignes ajoutées par ses commits | 41 717 (95 %) |

Un second agent (GLM, via ZCode) a écrit le reste : 26 commits,
2 207 lignes (5 %), dont le convertisseur web
(`tools/meta-compiler/web.mjs`), les palettes du boot ROM Game Boy Color
(runtime 0.7.0) et des corrections Pokitto. Il a aussi écrit l'émulateur
META/Pokitto qui a servi aux tests : il vit maintenant dans son propre
dépôt, [gm0](https://github.com/r043v/gm0).

Les chiffres viennent des transcriptions de Claude Code et de
l'historique git du développement. Le recompilateur de départ
(`recompiler/`, environ 17 000 lignes) est celui de gb-recompiled. Les
tests de performance sont des **estimations** faites avec un modèle de
cycles du M0+, pas des mesures sur console.

### Historique

Le dépôt m0boy garde l'historique de gb-recompiled (104 commits
d'arcanite24 et de ses contributeurs), puis celui du portage. Deux choses
en ont été retirées :

- l'émulateur (`tools/meta-emu-sdl`), devenu [gm0](https://github.com/r043v/gm0) :
  les commits qui ne touchaient que lui ont disparu (61) ;
- les premiers dossiers de test de gb-recompiled qui contenaient des ROMs
  commerciales converties en C (`pokemon_test`, `test_output_tetris`,
  `tetris_test`, `tetris_test_analysis`, `tetris_fixed`), et une capture
  audio de débogage (`debug_audio.raw`, `debug_audio.wav`).

## Consoles

| | Gamebuino META | Pokitto |
|---|---|---|
| Processeur | SAMD21, 48 MHz | LPC11U68, 48 MHz (72 MHz en option) |
| Écran | 160x128 : les 144 lignes du Game Boy ramenées à 128 | 220x176 : image 1:1 ou agrandie en 200x176 |
| Firmware | PlatformIO | `make` avec arm-none-eabi-gcc |
| Version | 0.7.0 | 0.7.0 |
| État | Jeux lancés sur console | Testé en émulateur. Un premier testeur sur vraie Pokitto signale une image à l'envers, cause en cours d'analyse |

Les deux consoles partagent le runtime, le code généré et le menu
d'options (HOME ou C maintenu) : sauvegardes d'état, volume, palettes
(dont celles du boot ROM GBC), saut d'images, vitesse, 72 MHz. Les ROMs
de 256 Kio et plus sont lues sur la carte SD au fil du jeu, sans limite de
taille.

## Démarrage rapide

Prérequis : CMake 3.20 ou plus, Ninja, gcc ou clang, Python 3, et
[PlatformIO](https://platformio.org/) pour la META. La Pokitto n'a besoin
que d'`arm-none-eabi-gcc` (celui du PATH, sinon celui de PlatformIO). Détails, options et dépannage : [META.md](META.md).

```bash
cmake -G Ninja -B build .
ninja -C build                 # build/bin/gbrecomp (et le runtime PC si SDL2 est installé)

# Mets tes ROMs dans roms/gb/, puis :
./compile.sh                   # demande la console (META par défaut)
TARGET=pokitto ./compile.sh    # ou directement la Pokitto
TARGET=both ./compile.sh       # ou les deux
```

Chaque jeu arrive dans `roms/meta/<Jeu>/` ou `roms/pokitto/<Jeu>/`. Copie
ce dossier tel quel à la racine de la carte SD, puis lance-le depuis le
loader de la console.

Tests :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build -R meta --output-on-failure
```

## Contenu

- `recompiler/` : `gbrecomp`, le recompilateur (gb-recompiled), avec
  l'émetteur des cibles META et Pokitto (`--target meta`, `--target pokitto`).
- `targets/meta/runtime/` : le runtime `gbl` (CPU de secours, PPU, APU,
  timers, mappers), commun aux deux consoles ; `gbl_profile.h` liste chaque
  optimisation par console.
- `targets/meta/platform/` : le frontend commun (menu, sauvegardes, carte
  SD, affichage) et les pilotes bare metal de chaque console.
- `compile.sh` : conversion en lot, profils de jeu, icônes, mises à jour
  de version.
- `tools/meta-compiler/` : le même convertisseur en Node, avec une page
  web locale.
- `tools/gb-emu/` : fork de [gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
  (Andy O'Neill, MIT), utilisé pour les parties de profilage.
- `targets/meta/README.md` : notes techniques et mesures (en anglais).

Le reste du dépôt (runtime PC SDL, ports, outils de précision) vient de
gb-recompiled : voir [README.gb-recompiled.md](README.gb-recompiled.md).
L'archive jointe à chaque release ne garde que le recompilateur, les
cibles M0+ et leurs outils.

## ROMs et droits

Aucune ROM et aucun firmware compilé ne sont fournis. Convertis seulement
des jeux que tu possèdes, ou des homebrews dont la licence le permet.

## Licence

MIT, voir [LICENSE](LICENSE). Basé sur [gb-recompiled](https://github.com/arcanite24/gb-recompiled),
© 2026 arcanite24, licence MIT. `tools/gb-emu` garde sa propre licence
MIT (© 2017 Andy O'Neill).
