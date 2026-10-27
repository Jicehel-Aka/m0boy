# SNES : état du noyau desktop

Le système `--system snes` produit pour l'instant un projet `--target desktop` :
CPU 65C816 interprété ou recompilé en C, bus LoROM/HiROM partiel, WRAM,
DMA général et premier rendu des fonds et des objets PPU. Ce n'est pas encore
une cible PC interactive ni un firmware pour les consoles. Le son, HDMA, les
modes graphiques 2 à 7, les fenêtres, la composition, les effets de couleur
et la SRAM ne sont pas implémentés. Le rendu prend les registres
à la fin de chaque image : il ne reproduit pas les changements en cours de
trame. Le timing CPU, DMA et vidéo est approximatif et n'a pas été comparé à
un matériel SNES. Le code recompilé ne reproduit pas encore tous les accès
de lecture d'instructions qui alimentent le bus ouvert : une ROM qui dépend
de cette valeur peut donc diverger de l'interpréteur.

Le PPU dispose de VRAM et CGRAM, des ports de transfert, de la luminosité et
des fonds à tuiles et des objets en modes 0 et 1. Les limites de 32 objets et
34 tuiles par ligne et les drapeaux correspondants ne sont pas émulés. Le DMA général prend en charge les modes
de transfert, la direction et l'incrément d'adresse, sans coût temporel.
La trame est découpée en lignes et déclenche la VBlank et la NMI ; HBlank,
IRQ et HDMA ne sont pas encore émulés. Un contrôleur est disponible en lecture
série ou automatique ; le temps de lecture automatique est approximé à trois
lignes. Les détails des ports et des priorités ne sont pas tous reproduits.

## Observation du démarrage

Le banc de référence se construit avec le projet :

```sh
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
build/bin/snes_host chemin/vers/cartouche.sfc --frames 120 --capture logs/snes.ppm
```

La capture est une image PPM 256 × 224. Les ROMs restent hors du dépôt.
`--input fichier.txt` accepte des lignes `frame boutons`, par exemple `0 start`
ou `5 right+b` ; les boutons ne sont actifs que sur la trame indiquée. Les noms
sont `b`, `y`, `select`, `start`, `up`, `down`, `left`, `right`, `a`, `x`, `l`
et `r`.
Sur les cartouches de diagnostic présentes localement dans `output/snes/`,
CPUMSC affiche son écran avec NOP, WDM, BRK, COP et WAI marqués PASS ; STP
demande un reset. CPUMOV et CPUADC affichent également des écrans de résultats.
Ces observations ne constituent ni une comparaison avec un émulateur de
référence ni une preuve de compatibilité des jeux.

Le jeu homebrew [Bucket](https://github.com/retrobrews/snes-games) a aussi été
essayé : après 120 images, le CPU exécute du code mais l'écran reste noir. Son
démarrage n'est donc pas encore validé.

Pour CPUMSC, un projet précis généré avec `--meta-code-budget 24000` a donné
à 120 images les mêmes empreintes CPU et image que son `--interp`, avec la
marche statique puis avec un profil d'exécution. Cette comparaison couvre
uniquement ces exécutions et les instructions effectivement recompilées ; les
autres restent exécutées par l'interpréteur.

Les tests synthétiques `ctest --test-dir build -R snes --output-on-failure`
vérifient notamment le bus, les interruptions, les appels longs, WAI, le DMA,
les ports vidéo, le rendu et la comparaison du projet généré avec l'interpréteur.
