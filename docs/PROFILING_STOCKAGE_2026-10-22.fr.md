# Profiling : ce qui reste en flash, et ce que coûte chaque accès

Les Game Gear, Master System et Game Boy plus grands que la flash lisent une
partie de leur ROM sur la carte SD, à travers le cache de blocs partagé
(`targets/common/runtime/m0_cache.inc`, la RAM de cartouche gardée sur la
carte y passe aussi).  Ce qui reste en flash est choisi par une simulation
de ce cache sur les parties enregistrées, avec le coût de chaque accès.

Toutes les valeurs ci-dessous sont des cycles comptés par gm0 ou des
simulations sur des traces hôte : des estimations, aucune mesure sur
console.

## Outils

- Traces (`--rom-trace-out`, format `m0boy.romtrace.v2`,
  `targets/common/host/m0_rom_trace.h`) : chaque lecture du jeu sous C000,
  dans l'ordre, au grain de 16 octets, chaque répétition comptée (chacune
  coûte une lecture au firmware), les octets d'instructions de
  l'interpréteur un par un, la RAM de cartouche (écriture : seulement quand
  l'octet change).  Le harnais Game Gear trace dans `gg_read` et `RD`
  (code compilé) ; le harnais de chaque projet généré
  (`cmake -DM0_ROM_TRACE=ON`) rejoue donc les parties avec le code compilé
  et le minutage du firmware.  Le harnais Game Boy (`_profile`) écrit le
  même format.
- `m0romsim` : le cache de `m0_cache.inc` rejoué (secteur en attente,
  CLOCK, voies protégées, RAM de cartouche, écriture différée de 120
  images), le coût de chaque classe d'accès (`--runtime current|lean`,
  `--costs`), caches séparés code/données/RAM (`--split`), blocs bruts
  sous 512 octets (`--sub`), LZSS de petits blocs (`--small-lzss`), blocs
  sans leurs zéros (`--zero`), pages mappées directement (`--page-direct`).
- `m0profile.py storage` : traces de toutes les parties (dix minutes au
  plus chacune), validation croisée, plan (`rom.plan`) et classement pour
  le générateur (`rom.rank`, `--rom-plan`), Game Gear, Master System et
  Game Boy ; `compile.sh` passe le projet généré du build précédent
  (`--project`).
- `m0profile.py calibrate` : coûts SD et LZSS par régression sur plusieurs
  longueurs d'une même partie dans gm0.
- gm0 `--play-guest` / `--until-guest` : une session rejouée par image du
  jeu, la même partie pour deux firmwares de vitesses différentes.

## Le simulateur contre le firmware

Ristar (META), ta session gm0 rejouée par image du jeu, 5 800 images :

| | gm0 (firmware) | m0romsim |
|---|---:|---:|
| Lectures SD | 933 | 933 |
| Décodages LZSS | 2 468 | 2 468 |
| Blocs chargés | 3 401 | 3 401 |
| Chemin ROM, cycles par image | 10 908 | 10 470 |

Super Mario Land 2 (META, cache unifié 61 voies, RAM de cartouche sur la
carte), une partie automatique, 6 000 images : 65 blocs ROM chargés des
deux côtés, 539 blocs de RAM de cartouche des deux côtés.

Le harnais générique (tout interprété) s'écarte du firmware vers l'image
3 500 de cette partie : le code compilé non précis n'a pas le même
minutage.  D'où les traces par le projet généré.

## Le vrai coût d'un accès

0.10.35, gm0, par instruction : `gg_stream_read` coûtait 81 cycles par
lecture, dont 62 pour sauver et restaurer r4-r11 (le chemin de défaut était
inliné) ; la copie d'un bloc de 64 octets passait par le `memcpy` octet par
octet de newlib-nano, 10,2 cycles par octet ; un hit sur le secteur en
attente, 111 cycles.

0.10.36 :
- `gg_stream_read` en assembleur, sans pile : un octet de secteur brut du
  pool en 35 cycles (cinq lectures en flash à 4 cycles dans gm0), le bloc lu
  en dernier en 41, sinon un saut terminal vers `cache_rom_fill` ;
- `cache_rom_fill` (secteur en attente, balayage) et `cache_rom_miss`
  (victime, chargement) séparés : un hit sur le secteur en attente coûte
  71 cycles ;
- copies de taille fixe en assembleur, `m0_copy.inc` : 16, 32, 64, 128,
  256 et 512 octets, source alignée (`ldm`/`stm`) ou décalée de 1, 2 ou 3
  octets (mots joints par décalages), déroulées jusqu'à 64 octets ; un bloc
  de 64 octets en 55 cycles au lieu de ~660.

Même partie, même pool, même code compilé (gm0, 5 800 images) : chemin ROM
21 942 → 10 273 cycles par image (puis 10 908 avec le profil de cette
session, nouveau code compilé) ; lectures SD, décodages et blocs chargés
identiques ; RAM, VRAM, palette et Z80 du jeu identiques à l'image 3 000.

Lecture SD : 17 254 cycles (régression sur 11 longueurs ; 3,57 M cycles au
démarrage, une fois ; les changements de vitesse SPI, ~2 255 cycles par
image, suivent l'écran et les boutons).  Décodage LZSS de 512 octets :
11 507.  Les générateurs Game Gear et Game Boy supposaient 50 000 et
16 000 ; ils prennent ces valeurs (`rom_storage.h`).

## Le pipeline complet (0.10.36)

Plan refait sur les traces du projet généré, coûts mesurés, générateur
avec les coûts SD et LZSS mesurés ; même partie (gm0, 5 800 images du jeu) :

| | runtime lean, ancien plan | 0.10.36 complet |
|---|---:|---:|
| Lectures SD | 933 | 818 |
| Décodages LZSS | 2 468 | 30 |
| Chemin ROM, cycles par image | 10 908 | 8 883 |
| LZSS, cycles par image | 4 894 | 46 |
| Interpréteur, cycles par image | 40 655 | 35 799 |
| Travail du CPU (attentes exclues), par image | 738 005 | 723 969 |
| Images gm0 pour 5 800 images du jeu | 5 906 | 5 861 |

Avec une lecture SD à 17 254 cycles au lieu de 50 000, le générateur
compile 126 instructions de plus (3 463) et garde 136 secteurs au lieu de
157, presque tous bruts.

## Sur console (META, 2026-10-22)

Firmwares de mesure (`GG_BENCH` : entrée scriptée, la partie gm0 rejouée
image du jeu par image du jeu, sans rythme ni image sautée, cache fixe de
12 voies), images 300 à 5 800, temps mesuré par la console
(`m0_micros`, lu par `tools/meta-usb.py`) :

| | HEAD (0.10.34) | 0.10.36, plan simulé | 0.10.36, plan par fréquence |
|---|---:|---:|---:|
| Temps (5 500 images) | 133,37 s | 130,82 s | 130,84 s |
| Images sans bloc chargé | 3 928 | 5 064 | 5 075 |
| Images de plus de 25 ms | 1 657 | 1 149 | 1 138 |
| Images de plus de 33,4 ms | 48 | 26 | 31 |
| Image la plus longue | 44,6 ms | 42,5 ms | 43,9 ms |
| Lectures SD | 599 | 559 | 537 |
| Décodages LZSS | 2 119 | 21 | 24 |
| Blocs chargés | 2 718 | 580 | 561 |
| Instructions compilées | 3 311 | 3 463 | 3 512 |

- Répétabilité : HEAD deux fois, 133,383 s et 133,369 s.
- gm0 refait les mêmes lectures SD, décodages et blocs chargés que la
  console ; il compte 8,3 à 8,7 % de temps en trop (145,0 et 141,7 s) :
  ses cycles surestiment, ses comptes sont exacts, ses écarts relatifs
  tiennent (−2,3 % dans gm0, −1,9 % sur console).
- Le gain vient du runtime (chemins d'accès, copies) et des coûts mesurés
  dans le générateur (plus de code compilé, presque plus de LZSS) ; le plan
  simulé ne fait pas mieux, sur cette partie, que les secteurs choisis par
  fréquence de lecture avec les mêmes coûts (écart de 0,02 %, du bruit).

## Tailles de blocs, codecs, organisation du cache

Plans appris sur les 14 parties automatiques, testés sur ta session (dix
minutes), budget du pool de Ristar (72 264 octets), coûts mesurés :

| Variante | cycles / image | SD / image |
|---|---:|---:|
| Secteurs de 512 octets et plus, cache 14 × 64 | 9 351 | 0,048 |
| + blocs bruts sous 512 (16 à 256) | 11 548 | 0,087 |
| + LZSS de blocs de 64 à 256 | 59 659 | 1,21 |
| + blocs de 32/64 sans leurs zéros | 12 891 | 0,174 |
| cache 56 × 16 | 10 276 | 0,042 |
| cache 28 × 32 | 9 687 | 0,051 |
| cache 7 × 128 | 10 463 | 0,110 |
| cache 2 × 512 | 15 742 | 0,428 |
| séparé : code 4 / données 10 | 9 974 | 0,083 |
| séparé : code 7 / données 7 | 11 201 | 0,145 |

- Les petits blocs collent aux octets lus pendant l'apprentissage ; une
  autre partie lit leurs voisins.
- Compresser de petits blocs fait décoder à chaque défaut, sans le secteur
  en attente qui servait les voisins : à proscrire.  Les données chaudes
  restent brutes : aucun bloc compressé dans le dixième le plus lu des
  plans, même quand les petits codecs sont permis.
- Sous 64 octets, le balayage des voies (8,3 cycles par voie) coûte plus
  que ce que les défauts évités rapportent ; au-dessus, trop peu de voies.
- Un cache unifié vaut mieux que des parties séparées à RAM égale : la
  séparation fige un partage que le CLOCK adapte.
- Aucune page n'était entièrement gardée brute : `--page-direct` ne change
  rien sur Ristar.

La taille minimale de bloc est donc choisie à partir de 512 octets, le
cache garde 64 octets, unifié.  Les blocs de 16 et 32 octets sont acceptés
par `m0_cache.h` (testés sur l'hôte, `meta_cache_unified_16/32`), pas
utilisés.

## Limites

- Un jeu Game Gear et un jeu Game Boy, une session humaine.
- Pokitto : mêmes chemins (le Game Gear y garde des blocs de 512 octets en
  RAM USB), coûts non mesurés dans gm0.
- Le Game Boy n'a pas encore de dossier de profil dans `profiles/gb/` ; le
  chemin `compile.sh` → `storage` → `--rom-plan` a été suivi sur une copie.
- Le coût d'une écriture SD (RAM de cartouche) est supposé deux fois celui
  d'une lecture.
