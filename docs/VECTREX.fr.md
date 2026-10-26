# Vectrex (expérimental) : META et PC

Le générateur accepte les cartouches `.vec` de 1 à 32 Kio (sans
commutation de banques) et produit un projet pour la META
(`--target meta`) ou un hôte SDL2 (`--target pc`). Le 6809 est recompilé
en C devant l'interpréteur (voir « Recompilation 6809 → C ») ; `--interp`
garde l'interpréteur seul.

Sur META, le générateur crée un fichier `.ST8` vierge de dix emplacements à
copier à côté du `.BIN` sur la carte. Le menu peut sauvegarder et recharger
l'état du 6809, de la VIA, du son et du phosphore. Le harnais PC accepte
`--save-state` et `--load-state` ; son test 60 = 30+30 trames retrouve le même
`run_hash`. Le firmware META compile et tient en RAM/flash ; le chargement
sur une console physique reste à vérifier.

```sh
cmake -G Ninja -B build . -DBUILD_TESTS=ON && ninja -C build
# META : la ROM système est intégrée au firmware généré
./build/bin/m0recomp 'roms/vectrex/Berzerk (1982).vec' --target meta \
    --bios 'roms/vectrex/Vectrex BIOS (1982).vec' -o output/vx_berzerk
pio run -d output/vx_berzerk
# PC : le BIOS est lu au lancement
./build/bin/m0recomp 'roms/vectrex/Berzerk (1982).vec' --target pc -o output/vx_pc
cmake -G Ninja -S output/vx_pc -B output/vx_pc/build && ninja -C output/vx_pc/build
output/vx_pc/build/vectrex_pc --bios 'roms/vectrex/Vectrex BIOS (1982).vec'
# profil : l'interpréteur compte les instructions par adresse, puis le
# projet recompilé les met en tête (et compile ce qu'elles atteignent)
output/vx_pc/build/vectrex_pc --bios 'roms/vectrex/Vectrex BIOS (1982).vec' \
    --interp --frames 3000 --coverage logs/berzerk.cov
./build/bin/m0recomp 'roms/vectrex/Berzerk (1982).vec' --target meta \
    --bios 'roms/vectrex/Vectrex BIOS (1982).vec' --coverage logs/berzerk.cov -o output/vx_berzerk
```

Côté PC, `--bios` au moment de générer est facultatif : avec lui, le code
du BIOS est compilé aussi, et l'hôte refuse le code natif (il interprète)
si la ROM système chargée n'a pas la même somme.

Sans `--bios`, la cible META prend l'unique fichier de 4 ou 8 Kio dont le
nom contient « bios » à côté de la cartouche. Un BIOS de 4 Kio (l'Executive)
est placé en F000 ; un fichier de 8 Kio (la ROM complète avec Mine Storm)
couvre E000-FFFF. Le BIOS et la cartouche ne vont que dans `output/` :
jamais dans le dépôt ni dans une archive. `--full-boot` garde l'intro du
BIOS sur la console (par défaut elle est raccourcie, comme sur PC).

## Organisation

- `targets/vectrex/runtime/` : le cœur partagé.
  - `m6809.c` : l'interpréteur de référence, une instruction à la fois, avec
    les cycles de la fiche technique (modes indexés compris).
  - `m6809_run.c` : la boucle rapide (registres dans la machine, bus par
    pages de 1 Kio) ; les cas rares (pages 0x10/0x11, EXG/TFR, RTI, SWI,
    SYNC/CWAI, interruptions, code hors ROM/RAM, opcodes illégaux) sont
    confiés à la référence.
  - `via.c` : la 6522 et l'analogique (DAC, échantillonneurs, intégrateurs,
    ZERO, BLANK). La VIA n'est mise à jour qu'aux accès du CPU (`via_sync`) ;
    le faisceau avance en ligne droite entre deux changements d'état et
    chaque trait est un appel `vx_draw()` du frontend. Une boucle qui ne fait
    que relire IFR (mêmes registres, aucune écriture) est avancée d'un coup
    jusqu'au prochain événement de la VIA.
  - `psg.c` : l'AY-3-8912 en entiers, par événements (prochaine bascule
    connue d'avance).
- `targets/vectrex/host/vectrex_host.c` : l'hôte SDL (image 512×512,
  `--frames N --ppm f --wav f`, affiche un `run_hash` des traits).
- `targets/vectrex/platform/vx_main.cpp` : la META. Le carré du faisceau est
  dessiné en 128×128 dans un phosphore 4 bits (8 Kio) centré sur l'écran ;
  la trame de 30 000 cycles tourne en 128 tranches et chaque tranche envoie
  une ligne d'écran puis la fait décroître (DMA en parallèle de
  l'émulation) ; les lignes restées noires ne sont pas renvoyées.
  Croix : manche ; A, B, MENU, HOME : boutons 1 à 4 ; HOME maintenu : menu.

## Recompilation 6809 → C

`recompiler/src/system/vectrex/emitter.cpp` écrit `src/vx_gen.c` ;
`targets/vectrex/runtime/vx_gen.h` en est le contrat, le répartiteur est
dans `m6809_run.c` sous `VX_NATIVE`.

- Ce qui est compilé : ce qu'atteignent les vecteurs du BIOS, l'entrée de
  la cartouche (après son en-tête « g GCE », la musique et les lignes de
  titre), les cibles connues, les sites de retour et le profil de l'hôte
  (`--coverage`). Sur la META, autant d'instructions que la flash en
  tient (~46 octets l'une, mesuré entre 42 et 50), les jouées d'abord.
- Les régions sont des suites d'instructions voisines (128 au plus), une
  fonction C chacune, un `switch` sur l'entrée en tête. Toute sortie revient
  au répartiteur (Thumb-1 n'enchaîne pas les appels terminaux) : vers une
  cible connue d'une autre région avec son numéro comme indice, sinon pc
  à rechercher (dichotomie sur les étendues des régions). RTS, PULS PC,
  JMP/JSR par registre ou page directe sortent ainsi ; SWI, RTI, CWAI,
  SYNC, les opcodes illégaux, le code en RAM restent à la boucle rapide,
  comme tout ce qui arrive quand `attention` est levé (interruption de la
  VIA permise, attente, démarrage raccourci).
- La sémantique est celle de la référence (`m6809.c`), cycle pour cycle :
  mêmes comptes, mêmes accès dans le même ordre ; un accès hors des pages
  (la VIA) voit l'horloge que l'interpréteur lui montrerait (début de
  l'instruction, cycles déjà comptés), pc après l'opérande quand la
  lecture peut être celle d'IFR. Les registres restent dans la machine
  (le détecteur de boucle d'attente de la VIA les compare) ; l'horloge et
  la fin de tranche sont des locales.
- Les octets de ROM (cartouche, BIOS) sont des constantes. La page
  directe passe par deux pointeurs rechargés quand DP change ; la pile, si
  le bloc tient dans la RAM, par des accès directs, sinon par
  `vxg_push`/`vxg_pull` dans l'ordre de la boucle rapide.
- Drapeaux : seuls ceux qu'un lecteur ultérieur peut voir sont calculés
  (vivacité sur le graphe des instructions compilées ; une sortie, une
  lecture qui peut être IFR, un empilement de CC les lisent tous). En fin
  de tranche, le CC rangé peut donc différer de l'interpréteur sur des
  drapeaux morts ; l'hôte de test compare tout sauf CC.
- `--meta-precise` vérifie la tranche après chaque instruction (même
  frontière que l'interpréteur, tout est entrée) : c'est le mode des
  comparaisons. Le mode rapide (celui de la console) ne vérifie qu'aux
  entrées, au plus 64 cycles d'écart ; une écriture d'IER s'y voit à
  l'entrée suivante.

## Vérifications

- `ctest -R vectrex_end_to_end` (`tests/test_vectrex_e2e.py`, sans ROM) :
  un BIOS synthétique, un programme qui trace par la VIA comme le BIOS
  (DAC, échantillonneurs, rampe sous T1, registre à décalage, trames T2,
  IFR scruté) avec piles, modes indexés, arithmétique 16 bits, DAA, MUL,
  EXG/TFR, branches longues ; puis 8 programmes aléatoires d'instructions
  valides (tous les modes, appels, empilements) : le projet précis donne
  la ligne de `--interp` (run_hash, registres sauf CC, somme de la RAM),
  depuis le parcours statique et avec le profil. Le test détecte les
  mutations essayées (drapeau H, cycles d'un mode indexé, vivacité de C
  pour ROR) ; il ne voit pas un pc manquant sur une lecture d'IFR (le
  balayage de la collection, si).
- Collection locale, 231 cartouches, 2 000 trames sans manette après un
  profil `--interp` de la même durée : projet précis = interpréteur
  (run_hash identique) sur les 231.
- `ctest -R vectrex_core_smoke` : instructions, cycles, DAA, traits de la
  VIA, T1, PSG ; puis les deux cœurs 6809 en lockstep sur du code aléatoire
  (CPU, RAM et VIA comparés après chaque instruction, 200 graines). Le test
  injecte des mutations qu'il détecte (cycles indexés, drapeau H, DAA, LEAY).
- Les optimisations (VIA paresseuse, saut des boucles d'attente, boucle
  rapide, registre à décalage par plages) ont été validées en comparant le
  `run_hash` de l'hôte sur 6 jeux × 2 démarrages, 1 500 trames chacun, et le
  WAV de 5 jeux, à l'identique avec la version précédente.
- Sur 231 cartouches de la collection locale, 228 tournent 3 000 trames sans
  opcode illégal (exceptions : deux démos « Battle Earth Terror Hazard » et
  Melody Master, qui utilise le crayon optique). Cela ne vaut pas test de
  compatibilité.
- Captures PC de Berzerk et Scramble inspectées : murs, robots, terrain,
  scores et textes du BIOS lisibles et à l'endroit.

## Vitesse sur la META

Mesures sur console (META branchée, compteurs `vx_frames`/`vx_busy_us`
lus par USB, mêmes trames 763-980 de la démo de Berzerk, mode rapide avec
le profil de 3 000 trames) :

| Berzerk, démo | images/s | émulation par trame |
|---|---|---|
| interpréteur (`--interp`) | 18,9 | 51,9 ms |
| 6809 recompilé | 28,4 | 34,4 ms |
| + VIA : état analogique en cache, registre à décalage par table, accès directs | 31,6 | 30,8 ms |
| + panneau : fondu par mot de 8 pixels, paires de pixels par table | 32,2 | 30,2 ms |
| + VIA en Thumb-1 (`via_asm.S`), écritures sans effet sautées | 36,6 | 26,6 ms |

La console tourne à 72 MHz : 26,6 ms font ~1,9 M cycles par trame, 50 Hz
en demandent 1,44 M. Le code en SRAM, qui échappe aux états d'attente de
la flash, est exclu : il a planté la META à 72 MHz
(`targets/meta/README.md`).

### La VIA en Thumb-1

`targets/vectrex/runtime/via_asm.S` (choisi par `vx_via_asm.h` sur
Cortex-M0+, `-DVX_VIA_ASM=0` garde le C) : `via_tick` (la boucle
d'événements, la signature analogique, la vitesse, le temps calme),
`via_write` pour ORB, ORA, T1H et SR (19 écritures sur 20), `via_read`
pour ORB et IFR, et les entrées lentes du code recompilé (`vxg_read`,
`vxg_write`) qui y mènent directement ; le reste passe à `via_write_c` /
`via_read_c`. Des `_Static_assert` dans `via.c` figent les décalages des
champs. Gain mesuré par gm0 : 7 à 11 % du temps de trame selon le jeu.
Côté C, une écriture qui ne change aucune entrée de la signature (ORA
mux coupé et rampe arrêtée, ORB sur les bits de son ou du mux, T1H déjà
lancé) ne recalcule plus rien.

Ce qui reste (profil gm0 de Berzerk) : `via_tick` ~16 % (≈1 350 tours de
boucle par trame, dont ~640 sans événement, le reste d'un intervalle après
le dernier), le code recompilé ~15 %, `via_write` ~9 %, le tracé et le
panneau ~12 %. Les événements du registre à décalage (deux par trait,
~580 par trame) sont la majorité : la suite serait de les éviter pour les
motifs pleins (0xFF), ce que le modèle actuel ne permet pas sans changer
les traits produits.

Vérification de l'assembleur (sans exécution possible sur l'hôte) : un
firmware construit avec `-DVX_DRAW_HASH` hache chaque trait et les
registres du PSG à chaque trame (`vx_draw_hash`, lu par
`gm0 --dump-ram`) ; la version C (`-DVX_VIA_ASM=0`) et la version
assembleur donnent le même hachage et la même horloge, trame de jeu par
trame de jeu, sur 13 cartouches, 900 trames, avec manette et boutons
(enregistrés une fois avec `gm0 --record-input`, rejoués par
`--play-guest`, qui suit la trame du jeu : une entrée par trame gm0
arriverait à d'autres trames de jeu sur un firmware plus rapide).

Les changements de la VIA gardent le résultat : run_hash de `--interp`
identique sur les 231 cartouches (2 000 trames), 11 démarrages complets,
WAV identiques sur 6 ; le test en lockstep des deux cœurs passe. Le
panneau : même table de fondu vérifiée sur 100 000 mots, images gm0
identiques à trame de jeu égale hors des lignes déjà envoyées de la trame
suivante.

### Avant la recompilation (estimations gm0, pas des mesures sur console)

Temps d'émulation par trame, 50 Hz demandent 20 ms (gm0, firmware à
72 MHz) :

| Jeu | 600 premières trames |
|---|---|
| Berzerk (400 trames) | 29,5 ms |
| Scramble | 39 ms |
| Star Castle | 41 ms |
| Mine Storm II | 39 ms |

Le jeu tournait donc à environ la moitié de sa vitesse ; le profil donnait
~45 % pour le 6809, ~30 % pour la VIA et le tracé.

## Limites connues

- Cycles du 6809 exacts par instruction, mais modèle analogique simplifié
  (pas de dérive, d'offset ni de saturation réaliste des intégrateurs).
- Les registres du PSG sont lus une fois par trame.
- Pas d'imageur 3D, de crayon optique ni de banques (cartouches > 32 Kio).
- Ni compatibilité de la collection, ni fidélité graphique, sonore ou
  temporelle ne sont établies.
