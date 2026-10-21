# Nombre de parties et durée du profiling

Cette campagne précède le balayage partagé de la version 0.10.30.
Voir la [réévaluation après ce changement](PROFILING_AFTER_STATIC_SCAN_2026-10-21.fr.md) :
ces gains de couverture ne constituent pas des gains de vitesse démontrés
sur la chaîne actuelle.

L'amélioration des sauvegardes a été commitée dans `f910da6`. Cette seconde
expérience multiplie le nombre de sessions par 2 et 4, et leur budget
d'images par 4, en partant des 2 sessions de 24 000 images du premier essai.

## Des graines différentes lançaient la même partie

Le harnais GG/SMS et celui du SG-1000 initialisaient `--random` avec
`seed | 1`. Les graines 2 et 3, 4 et 5, etc. devenaient identiques. Avant
correction, les profils Ristar des graines 2 et 3 étaient identiques octet
pour octet. Huit sessions numérotées de 1 à 8 ne donnaient donc que cinq
graines distinctes ; quatre sessions n'en donnaient que trois.

La lecture de la graine préserve maintenant les valeurs non nulles, et
`--random 0` conserve son comportement antérieur de graine 1. L'explorateur
commun ne force plus non plus le bit faible après son mélange initial :
cela créait aussi des collisions entre graines éloignées, notamment 1 et
`0xf174d0b0`. La multiplication par une constante impaire et le xorshift
préservent désormais les graines non nulles distinctes.

Les anciens profils restent utilisables. `AUTO_REDO=1` permet de refaire
les sessions qui contenaient ces doublons. La comparaison de checkpoints
du premier rapport précède cette correction de graines ; ses nombres ne
doivent pas être mélangés avec ceux de la présente expérience.

## Protocole

- Quatre jeux GG : Ristar, Sonic Triple Trouble, Shinobi et Columns.
- Même harnais corrigé, GCC, CMake/Ninja, mode Release (`-O3`).
- Graines 1 à 8, avec `--explore 24000` puis `--explore 96000` : 64 sessions,
  soit 3 840 000 images demandées au total.
- Pour chaque jeu, union des positions de code et des secteurs ROM lus,
  sur les graines 1 à N, avec N = 2, 4 ou 8. Les totaux additionnent les
  unions par jeu ; une position de code est une page et une adresse.
- Coût : somme du temps CPU utilisateur et système des harnais, mesuré
  par `resource.getrusage(RUSAGE_CHILDREN)` dans chaque processus de travail.
  Ce n'est ni une estimation M0 ni une mesure sur console. Les essais
  tournent sur quatre workers ; le temps mural subit aussi les tests en
  parallèle et n'est pas utilisé pour comparer les coûts.

Le budget est celui d'une session d'exploration complète, pas celui d'un
segment continu : les durées des segments et le stock de snapshots restent
inchangés. Chaque session finit son segment après avoir atteint son budget,
ce qui ajoute quelques images. Les profils bruts, métriques par session et
journaux sont dans `logs/profile-scaling-fixed/` ; le script de mesure et
la construction dédiée sont dans `out/profile-scaling/`, tous ignorés par Git.

## Résultats

Sommes des unions par jeu ; écarts par rapport à 2 × 24 000 images.
Le coût CPU porte sur les quatre jeux, pas sur une seule ROM.

| Sessions × images par jeu | Code | Gain code | Secteurs lus | Gain données | CPU total (s) |
|---|---:|---:|---:|---:|---:|
| 2 × 24 000 | 38 484 | référence | 1 006 | référence | 60,3 |
| 4 × 24 000 | 40 744 | +5,9 % | 1 091 | +8,4 % | 123,2 |
| 8 × 24 000 | 41 138 | +6,9 % | 1 108 | +10,1 % | 245,2 |
| 2 × 96 000 | 42 010 | +9,2 % | 1 126 | +11,9 % | 235,9 |
| 4 × 96 000 | 42 664 | +10,9 % | 1 151 | +14,4 % | 474,2 |
| 8 × 96 000 | 43 841 | +13,9 % | 1 190 | +18,3 % | 952,3 |

À budget demandé égal, 2 longues couvrent 2,1 % de code et 1,6 % de données
de plus que 8 courtes, avec 3,8 % de CPU en moins. Les dépassements de fin de
segment favorisent pourtant les courtes : 829 820 images jouées sur les
quatre jeux contre 783 668 pour les longues, soit 5,9 % de plus.

Quadrupler les deux paramètres demande 16 fois le budget initial et coûte
15,8 fois le CPU, pour +13,9 % de code et +18,3 % de données. Sur les longues,
passer de 2 à 8 sessions coûte environ quatre fois plus pour +4,4 % de code.
Le rendement marginal diminue donc fortement.

| Jeu | Code, 2 courtes | Code, 8 courtes | Code, 2 longues | Code, 8 longues |
|---|---:|---:|---:|---:|
| Ristar | 11 330 | 12 324 | 12 669 | 12 693 |
| Sonic Triple Trouble | 13 369 | 13 959 | 14 292 | 15 526 |
| Shinobi | 9 877 | 10 665 | 10 826 | 11 399 |
| Columns | 3 908 | 4 190 | 4 223 | 4 223 |

Le nombre de sessions reste utile sur Sonic (+8,6 % entre 2 et 8 longues),
mais presque plus sur Ristar (+0,2 %) et plus du tout pour le code de
Columns dans cet essai. Pour ce budget et cet échantillon, allonger d'abord
les sessions est donc plus rentable que multiplier les départs courts ;
un multiplicateur global élevé coûte cher pour un gain limité.

## Validation et limites

Le test unitaire vérifie les graines 1 à 8 et la collision éloignée, leur
reproductibilité et leur diversité de séquences d'entrée. Le test e2e GG/SMS
vérifie aussi que les graines CLI 2 et 3 produisent des hashes distincts sur
sa cartouche synthétique.

Le test unitaire CMake/CTest et les tests e2e Game Boy, GG/SMS et SG-1000
réussissent. La campagne a produit 64 profils complets et 3 989 564 images
effectivement jouées. Pour chacune des 32 paires de même jeu et même graine,
les ensembles code/data du long profil contiennent tous ceux du court,
ce qui vérifie que l'allongement prolonge bien le même parcours.

Cet échantillon ne mesure pas la vitesse des firmwares ni leur compatibilité.
Les gains dépendent du jeu et du choix des graines. Les réglages par défaut
de `compile.sh` restent inchangés : notamment, les quatre explorations GG/SMS
de 120 000 images chacune ne sont pas exactement une case de cette matrice.
