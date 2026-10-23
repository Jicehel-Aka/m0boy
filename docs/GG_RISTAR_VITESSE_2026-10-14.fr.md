# Game Gear : Ristar, ralentissements et sauts d'images — 14 octobre 2026

Symptôme : sur Ristar, la Game Gear saute des images, ralentit fortement et
le son suit. Les mesures ci-dessous viennent de **gm0** (cœur Cortex-M0+,
LCD, carte SD et timers émulés), pas d'une console. Le coût d'une lecture de
secteur de la carte SD y a été **supposé** (voir les limites). Versions
**0.10.4** (profil, secteurs en flash, LCD Pokitto) puis **0.10.5** (horloge
de la META, profils explorés : dernière section).

## Ce qui se passait

Banc : firmware complet de Ristar (Game Gear USA, 512 Kio, 26 pages de 16 Kio
lues sur la carte), touches scriptées par image Game Gear, de l'écran titre
au premier niveau ; la fenêtre mesurée est le jeu dans le premier niveau
(700 images). Pokitto à 72 MHz, le réglage du firmware que `compile.sh` a
produit pour ce jeu (`hc: 1`). Le Pokitto à 48 MHz n'a que 800 000 cycles
par image : il n'y atteindrait pas le plein régime, même après ces
changements.

Trois causes, vérifiées sur ce banc :

1. **Le profil de compilation ne voyait pas le jeu.** Les parties
   automatiques de `compile.sh` (3 600 images) arrivaient à peine au premier
   niveau, et le générateur classait les instructions *fichier par fichier* :
   les 3 891 instructions du premier fichier (logos et démo) remplissaient
   tout le budget de flash (3 686 instructions compilées). Le code du jeu
   tournait dans l'interpréteur : **1 117 instructions interprétées par
   image** (hôte, même script), à environ 300-400 cycles chacune, soit de
   l'ordre du tiers du budget d'une image à 72 MHz.
2. **Les données de ROM étaient lues sur la carte, secteur par secteur.** Les
   pages gardées en flash étaient choisies par page entière sur ce même
   profil. Dans le niveau, **au moins 6,5 secteurs de 512 octets étaient lus par
   image** en moyenne (borne basse tirée de l'histogramme ; 9 ou plus dans
   43 % des images), parce qu'une
   cinquantaine de secteurs éparpillés dans toutes les pages servent en
   boucle et dépassent le cache de quatre secteurs : 6 596 lectures depuis le
   démarrage, une lecture coûtant de l'ordre de la milliseconde sur une vraie
   carte.
3. **Adressage du LCD du Pokitto** : `at()` écrit trois registres par ligne
   depuis la flash (états d'attente), environ 470 cycles par ligne, 6,3 % du
   cycle d'une image.

## Changements

- **Classement par exécutions par image.** `gg_host` écrit `# frames N` et
  le nombre d'exécutions de chaque instruction interprétée. Le générateur
  classe par exécutions par image moyennées sur les parties, chacune pesant
  autant (`recompiler/src/system/sg1000/generator.cpp`, `read_coverage`). Sans
  comptes (profils d'avant), l'ancien classement reste.
- **Réserve de secteurs chauds en flash** (`GG_POOL_SECTORS`, `gg_pool`,
  `gg_pool_index`). La page 0 reste ; le reste de la flash sert aux
  instructions compilées *et* aux secteurs de 512 octets qui font gagner le
  plus de cycles par octet, selon les taux du profil (une instruction
  interprétée coûte environ 300 cycles de plus ; une lecture de secteur est
  comptée 50 000 cycles, estimation, pas une mesure sur console). Un secteur
  de la réserve se trouve par un index d'un octet par secteur de ROM, sans
  toucher à la carte. Sans taux, l'ancien choix de pages entières.
- **`compile.sh`** : parties Game Gear et Master System de 10 800 images
  (9 fois `AUTO_SECONDS` au lieu de 3 fois) ; un profil sans `# frames` est
  refait tout seul.
- **Pokitto : `at()` en assembleur Thumb-1 depuis la SRAM**
  (`pokitto_lcd_asm.S`), mêmes écritures et mêmes impulsions WR que la
  version C (deux `nop` entre CLR et SET), sans attente de flash.
- `line_sprites` : quatre octets copiés sans appel à `memcpy`.
- Mesure : `GG_BENCH` dans `gg_main.cpp` (voir plus bas).

## Mesures (gm0)

Fenêtre de 700 images du premier niveau. « Non cadencé » : le firmware ne
saute aucune image et n'attend pas ; « cadencé » : le firmware de jeu, avec
ses attentes et ses images non dessinées. Une lecture de secteur coûte en
plus 72 000 cycles (Pokitto) ou 60 000 cycles (META) dans gm0 : **supposition**.

| Pokitto 72 MHz | avant | après |
|---|---:|---:|
| ms par image, non cadencé | 32,3 | **15,8** |
| images de plus de 25 ms (sur 700) | 638 | 11 |
| vitesse du jeu, cadencé | 38,6 im/s (64 %) | **59,9 im/s (100 %)** |
| images non dessinées, cadencé (sur 700) | 466 | 71 |
| secteurs lus depuis le démarrage | 6 596 | 487 |
| instructions interprétées par image (hôte) | 1 117 | 69 |

| META 48 MHz | avant | après |
|---|---:|---:|
| ms par image, non cadencé, toutes dessinées | 49,9 | **26,3** |
| vitesse du jeu, cadencé | 25,0 im/s (42 %) | **58,2 im/s (97 %)** |
| images non dessinées, cadencé (sur 700) | 467 | 459 |

Étapes sur le Pokitto, mêmes banc et scénario (non cadencé, ms par image) :
32,3 → 21,4 (classement par exécutions) → 16,6 (réserve de secteurs, 50 % de
la flash au code) → 15,8 (arbitrage code / secteurs, `at()` en assembleur,
`memcpy`). Ces étapes intermédiaires viennent de constructions
successives du même banc.

Parties aléatoires de 8 000 images (`gg_host --random`, surtout des menus),
cadencé, Pokitto : graine 101, 56,3 → 59,7 im/s (660 → 133 images non
dessinées) ; graine 102, 59,1 → 59,7 im/s (272 → 102).

Ce qui reste visible au profil Pokitto dans le niveau : l'envoi au LCD
(`pokitto_push200`, 26 %), le fond (`gg_bg_tiles`, 24 %), puis le rendu C
(5 %) ; l'interpréteur ne pèse plus qu'environ 3 %.

Sur la META le jeu seul prend 10,9 ms par image ; dessiner ajoute 15 ms
(8,8 ms de rendu, 6,3 ms d'attente du bus du LCD : 128 lignes à 24 MHz font
13,7 ms de bus, sans recouvrement avec le jeu, deux tampons de ligne). Le jeu
et le son tournent donc à pleine vitesse, mais environ une image sur trois
seulement est dessinée. Des essais avec quatre tampons de ligne et
`GG_RENDER_STEP=4` ramènent 26,0 à 22,7 ms non cadencé, mais n'ajoutent que
quelques images dessinées en cadencé : non retenu (704 octets de RAM sur une
RAM à 88,6 %).

## Vérifications

- CTest : 28 tests, dont `gg_native_end_to_end` : nouveau cas à 320 Kio
  (réserve de secteurs et carte, natif précis = interpréteur) et profil au
  nouveau format.
- Collection locale : **1 121 sur 1 121** ROMs GG et SMS, 360 images,
  `--random 42 --start-frame 30` : interpréteur de la version précédente =
  interpréteur actuel (état, capture, WAV) et code natif précis =
  interpréteur. Le contrôle compile en desktop : il n'exerce ni la réserve de
  secteurs ni l'assembleur du Pokitto.
- Firmwares complets de Ristar (Pokitto et META, flash 98,9 % à 100 %),
  Garfield (GG) et Sonic Blast (SMS, Pokitto) construits par `compile.sh` ;
  Garfield et Sonic Blast démarrent dans gm0 et affichent leur titre.

## Limites

- Aucune mesure sur console. Le coût d'une lecture de secteur (72 000 ou
  60 000 cycles en plus) est une supposition ; il dépend de la carte.
- Ristar seulement pour la vitesse. Le premier niveau n'est pas tout le jeu :
  le code et les secteurs jamais vus pendant les parties automatiques (autres
  niveaux, combats de boss) restent interprétés ou lus sur la carte.
- `at()` en assembleur n'a pas été essayé sur la dalle du Pokitto ; les
  impulsions WR gardent la largeur de celles des pixels (trois cycles).
- Un profil enregistré avant cette version est refait à la prochaine
  compilation ; avec `AUTO=0` il n'est pas refait et garde l'ancien
  classement.
- La Game Gear du META dessine environ une image sur trois sur ce niveau
  (le bus du LCD).

## Refaire la mesure

Les outils de lecture (gm0 modifié : lecture d'un symbole de la RAM en fin
d'exécution, coût d'une lecture SD, profil `EMU_PROF`) ne sont pas dans ce
dépôt. Le firmware de banc, lui, est dans `targets/gamegear/platform/gg_main.cpp` :

```sh
# gg_bench.h dans src/ du projet généré : GG_BENCH_FROM, GG_BENCH_TO et
# GG_BENCH_INPUT ("image:masque:durée,...") ou GG_BENCH_SEED (= gg_host --random)
cmake -S out/gg/pokitto/Ristar -B out/gg/pokitto/Ristar/build \
  -DEXTRA_DEFS="-DGG_BENCH=1;-DPOKITTO_HC=1"        # META : PLATFORMIO_BUILD_FLAGS="-DGG_BENCH=1"
# options : -DGG_BENCH_PACED=1 (cadencement et sauts d'images réels),
#           -DGG_BENCH_NOVIDEO=1 (aucune ligne dessinée), -DGG_BENCH_NOLCD=1 (META : lignes
#           dessinées, non envoyées)
```

Le résultat est dans `gg_bench` (images et microsecondes, histogramme des
secteurs lus par image, images de plus de 16,7 / 25 / 33,4 ms, image la plus
longue, images non dessinées) et `gg_bench_window` (16 parts de la plage).

## 0.10.5 : horloge 72 MHz aussi sur META, profils explorés

### Horloge

La vitesse choisie dans le convertisseur vaut désormais pour les deux
consoles : `OVERCLOCK=1` (ou la question de `compile.sh`, une seule fois ;
`POKITTO_HC` reste accepté) définit `META_OVERCLOCK`, et chaque firmware
démarre à 72 MHz (`M0_CLOCK_DEFAULT`) ; le menu « Processeur » garde la
bascule 48 <-> 72 et le loader est toujours rendu à 48 MHz. Sur la META c'est
le chemin FDPLL96M déjà écrit dans `meta_clock.cpp` (deux états d'attente de
flash) : le SAMD21 est donné pour 48 MHz et **ce mode n'a jamais tourné sur
une META** (README, `meta_clock.cpp`). Le `.gbrmeta.json` garde `hc: 1` pour
les deux consoles, et un changement d'horloge fait refaire le jeu.

Dans gm0, la META ne suivait pas ce changement d'horloge : j'ai complété une
copie locale (FDPLL verrouillé, GCLK0 sur le FDPLL, SysTick à 72 000 cycles
par ms), sans toucher au dépôt gm0. Le timer audio y reste cadencé à 48 MHz,
l'interruption audio coûte donc un peu moins de cycles par image que sur la
vraie puce. Ristar, premier niveau, lecture de secteur à 90 000 cycles :

| META | 48 MHz | 72 MHz |
|---|---:|---:|
| ms par image, non cadencé | 26,3 | **17,6** |
| vitesse du jeu, cadencé | 58,2 im/s | **59,9 im/s** |
| images non dessinées, cadencé (sur 700) | 459 | **261** |

### Profils

Le profil de Ristar était bon parce que je l'avais guidé ; ce n'était pas
le cas de tous les jeux. Mesure, sur des parties de contrôle jamais vues
(graines 201 et 202, 20 000 images, firmware Pokitto, code natif sur hôte) :
instructions interprétées par image / secteurs lus sur la carte pour 1 000
images, graine 201. « Avant » : parties à l'aveugle de 10 800 images ;
« après » : plus quatre explorations, et le code que l'interpréteur lit sur la
carte compté dans le choix des secteurs.

| Jeu | avant | après |
|---|---:|---:|
| Alex Kidd in Miracle World (SMS) | 3 196 / 0 | **17** / 0 |
| Mortal Kombat | 239 / 3 397 | 377 / **569** |
| Garfield | 178 / 3 267 | 232 / **1 595** |
| Sonic Chaos | 78 / 390 | 93 / **36** |
| Bubble Bobble | 15 / 157 | 29 / **15** |
| Land of Illusion (Mickey) | 15 / 213 | 22 / **39** |
| Shinobi | 52 / 153 | 57 / **37** |
| Streets of Rage 2 | 17 / 58 | 18 / **19** |
| Ristar | 27 / 35 | 34 / **29** |
| Sonic the Hedgehog | 5 / 12 | 6 / **3** |
| Aladdin | 244 / 17 | 244 / **14** |

Ces contrôles sont aléatoires comme les parties « avant » : ils ne mesurent
pas le jeu réel ; la graine 202 va dans le même sens pour les secteurs lus
(Mortal Kombat 3 418 → 535) et pour Alex Kidd (3 205 → 15). Trois choses font la différence :

1. **Une exploration guidée par la couverture** (`gg_host --explore N`,
   `--random` pour la graine) : segments de 120 à 600 images avec une main
   plausible (direction tenue 8 à 64 images, boutons tenus ou tapés, START une
   fois sur cinquante), repartis d'un instantané de la machine pris quand un
   segment a vu du code ou un secteur de ROM jamais vu (48 instantanés, le
   moins repris d'abord, le départ de temps en temps). Le profil est celui de
   toutes les images jouées. Seule, elle atteint plus de code que les parties
   à l'aveugle sur un jeu sur deux et un peu moins sur les autres, d'où les
   deux ensemble. Sans START (0 %) la couverture tombe de moitié. 30 000 images
   donnent environ 96 % de ce que donnent 60 000.
2. **Le code de ce qui n'est pas compilé** : une instruction interprétée lit
   ses octets dans la ROM, donc sur la carte quand son secteur n'est ni en
   flash ni dans le cache. Le profil ne le voyait pas (il ne trace que les
   lectures de données) ; le générateur estime maintenant, pour chaque secteur,
   la probabilité qu'une image en lise le code des instructions non compilées
   et refait son choix (trois passes) avec ces secteurs.
3. **Le comptage.** Sur Mortal Kombat les secteurs les plus lus étaient du
   code : 3,4 lectures par image au lieu de 0,6.

Pokitto 72 MHz, firmware cadencé, graine 201 sur 9 000 images (ancienne
version : parties à l'aveugle de 3 600 images, ordre par fichier) : images non
dessinées, avant → après : Sonic the Hedgehog 2 000 → 64, Shinobi 2 754 → 833,
Aladdin 631 → 189 (la vitesse moyenne reste 59,7 im/s : ces parties aléatoires
sont surtout des menus).

Reste à savoir : Mortal Kombat et Garfield lisent encore 0,6 à 1,6 secteur par
image (le jeu parcourt plus de ROM que la réserve n'en tient), et un jeu qu'un
joueur automatique ne sait pas démarrer reste hors de portée. Le temps de
profil passe de quelques secondes à une vingtaine de secondes par jeu (quatre
explorations de 30 000 images, en parallèle).

## 2026-10-15 : cache de dispatch négatif, essayé et écarté

`sg_gen_lookup` (code généré) cache les adresses **trouvées** dans une table
directe de 32 places ; une adresse sans code compilé repasse par la
recherche binaire dans `entry_pc` (440 entrées sur Ristar, ~9 tours).

Mesuré sur Ristar, harnais hôte du projet généré, 2 400 images, entrée
`20:64:5,30:16:10,40:8:20` : 218 461 appels au dispatch, 144 299 servis par
le cache, **74 162 recherches binaires** (≈31 par image), `run_hash`
inchangé (2fec1b3d) et `out_mismatch=0`.  Le chemin négatif n'est donc pas
rare dès qu'une partie du code reste interprétée : c'est là que le coût se
cache, pas dans les positifs.

Version naïve (négatifs rangés dans le *même* cache de 32 places, marqueur
`(sg_fn)1`) : **1 012 746 341 instructions hôte contre 1 011 880 209**
(+0,09 %, `perf stat -e instructions:u`, même image) — les négatifs
chassent des positifs et la recherche binaire revient ailleurs.  Écartée.

La variante à cache négatif *séparé* (32 places à part, indexées par un
second hachage) a été mesurée proprement le même jour, la première mesure
étant fausse (un store placé après le `return 0` du chemin négatif faisait
répondre « interpréteur » pour des adresses compilées : +23 %
d'instructions, essai invalide).  Résultat : **1 702 461 334 instructions
contre 1 702 107 123** sur 4 000 images, soit **+354 211 (+0,02 %)**,
`run_hash` inchangé.  Compteurs posés dans la fonction, même partie :
380 990 appels, 269 043 servis par le cache positif (70,6 %), 96 430
recherches binaires, et **15 517 résultats négatifs servis par le cache,
soit 13,9 % des 111 947 réponses négatives**.  Chaque appel qui rate le
cache positif paie la vérification (~8 instructions, estimation) ; chaque
succès évite une recherche binaire, ~35 instructions compte tenu de
l'écart mesuré : le seuil de rentabilité est vers **23 % de succès**, pas
14 %.  Le chemin négatif est chaud mais **peu répétitif** — les adresses
interprétées se suivent plus qu'elles ne se répètent.  Les deux variantes
sont écartées et `sg_gen_lookup` reste inchangé : les 218 461 appels de
l'instrumentation précédente coûtent moins que n'importe quel cache de
leurs résultats négatifs.

## 2026-10-15 : couverture, deux leviers mesurés

Ce qui n'est jamais atteint pendant les parties automatiques (autres niveaux,
combats de boss) reste interprété : c'est la limite laissée ouverte plus haut.
Six jeux GG mal couverts (Aladdin, Garfield, Mortal Kombat, Ristar, Shinobi,
Sonic Chaos), 120 000 images d'exploration — le budget réel de `compile.sh`
(`AUTO_SECONDS=20` : `20 * 6000`) — et la métrique « locations » du joueur
automatique : les adresses de code distinctes qu'il atteint.  `--random N`
force la graine à un nombre impair, deux graines paires voisines jouent donc
la même partie.

### Segments plus longs

Un segment dure `M0X_SEGMENT_MIN` plus 0 à `M0X_SEGMENT_SPAN` images ; un sur
huit repart du démarrage de la console.  Moyenne de deux séries de quatre
graines (les réglages se suivent, la seconde série départage les deux
derniers) :

| locations, contre 480 + 1920 | 960 + 2880 | 1920 + 3840 | 2880 + 5760 |
|---|---:|---:|---:|
| Aladdin | +1,2 % | +2,5 % | +2,3 % |
| Garfield | +2,8 % | +1,1 % | +3,2 % |
| Mortal Kombat | +2,0 % | +2,4 % | +4,1 % |
| Ristar | +0,8 % | +3,6 % | +6,5 % |
| Shinobi | +4,2 % | +4,0 % | +5,5 % |
| Sonic Chaos | +3,1 % | +4,5 % | +0,8 % |

Les écarts par jeu valent ±5 % pour quatre graines : les trois réglages se
valent, tous au-dessus du défaut.  Retenu : le pas le plus petit,
**1920 + 3840**, et seulement dans le harnais GG/SMS (`gg_host.c`) — les
autres consoles n'ont pas été mesurées ici.

Écartés : `M0X_POOL` 96 (la réserve d'instantanés n'est jamais pleine à ce
budget : mêmes locations que 48, au chiffre près), `M0X_RESTART` 4 (un
segment sur quatre repart du début : mitigé, Ristar −9 %), START plus souvent
(8 % des changements de direction au lieu de 2 %) et START + bouton 1 en
rafale sur les segments repartant du démarrage (essais à 30 000 images) :
les deux derniers font moins de locations que le défaut sur cinq jeux sur six,
jusqu'à −20 % — la pause de START coûte plus qu'elle ne rapporte.

### Les profils de la collection sont restés en v1

Les 225 profils GG de la collection locale datent du 13 octobre : format v1
(sans « # frames », sans « # read »), aucune exploration.  Le générateur ne
peut alors ni classer les instructions par exécutions par image ni garder les
secteurs utiles, et `compile.sh` les refait à la prochaine compilation (un
profil sans « # frames » est périmé).

Mesure : même générateur, mode précis (`--meta-precise` ; sans lui le natif et
l'interpréteur divergent sur ces jeux et la comparaison n'a plus de sens),
parties de contrôle de 20 000 images, graines 201 et 202.  Les trois variantes
jouent exactement la même partie (`run_hash` identique) ; seuls les profils
changent.  Moyenne des deux graines :

| | Mortal Kombat | | Garfield | | Aladdin | |
|---|---:|---:|---:|---:|---:|---:|
| | interp./image | SD/1000 | interp./image | SD/1000 | interp./image | SD/1000 |
| v1 (collection) | 1 021 | 5 893 | 1 309 | 6 903 | 443 | 272 |
| v2, sessions auto | 348 | 985 | 164 | 1 286 | 262 | 24 |
| v2 + 4 explorations | 351 | 967 | **137** | **486** | 261 | 20 |

Le format v2 vaut à lui seul 1,7× à 8× moins d'instructions interprétées et 5
à 11× moins de lectures SD ; les explorations ajoutent surtout sur Garfield
(−16 % d'interprété, −62 % de lectures) et rien de visible ici sur Mortal
Kombat ni Aladdin (profil déjà complet, flash plein à 97 %).  Les deux termes
sont ceux que le générateur compare (300 cycles par instruction interprétée,
50 000 par lecture : des estimations, pas des mesures de console).
