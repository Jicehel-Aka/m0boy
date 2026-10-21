# Pertinence du profiling après le balayage statique partagé

Analyse de `main` à `5c05929` (balayage ajouté dans `e5bd063`, version
0.10.30), comparé aux changements `f910da6` (checkpoints) et `2ff291f`
(graines et étude du nombre/durée des sessions).

Les nouveautés étaient sur `main`, pas sur `feat/espboy`. Elles ont été
récupérées et étudiées dans le worktree isolé `out/profile-review/`.
Les branches n'ont pas été fusionnées dans le répertoire de travail principal.

## Verdict

- **Conserver la correction des graines.** `main` force toujours les graines
  GG/SMS et SG-1000 à être impaires : 2 et 3 lancent la même partie. Le
  balayage statique ne corrige pas ce défaut, qui gaspille des sessions.
- **Les checkpoints restent complémentaires, mais leur bénéfice de vitesse
  est à relativiser.** Le balayage retrouve une partie de leur code nouveau.
  Ils en découvrent encore qui n'était pas sélectionné, mais les essais hors
  profil ci-dessous donnent un effet presque neutre sur l'interprétation.
  Les anciens gains de couverture ne sont pas des gains de vitesse mesurés.
- **Ne pas augmenter globalement le nombre et la durée des sessions sur la
  seule base des anciens pourcentages.** Le nouveau balayage et le budget
  limitent davantage le bénéfice marginal. Les réglages par défaut n'avaient
  pas été modifiés ; il n'y a pas lieu de les augmenter avec cette preuve.

Le profiling reste particulièrement utile pour les fréquences d'exécution,
les cibles indirectes et banques réellement utilisées, et les lectures de
données qui guident le placement des secteurs ROM. Le balayage classe des
octets ; il ne mesure pas leur consommation pendant le jeu.

## Ce que fait la nouvelle sélection

Dans `sg1000/emitter.cpp`, `Emitter::run` prend les instructions jouées,
classées par fréquence, puis suit leurs destinations statiques. Le balayage
spéculatif ajoute ensuite des instructions tant qu'il reste du budget.
Les instructions jouées conservent donc leur priorité. Les fréquences
servent aussi au choix des pages habituelles et à l'expansion des helpers
mémoire/pile aux sites chauds. Le générateur utilise les lectures par frame
pour arbitrer code compilé et secteurs ROM résidents/compressés.

Sur Game Boy, les zones de données restent des filtres pour les semeurs
spéculatifs ; elles ne remplacent pas les preuves d'exécution réelles.
Le parcours dynamique et les profils data restent pertinents sur les deux
familles de consoles.

## Mesures avec le nouveau balayage

56 projets GG ont été générés pour Ristar, Sonic Triple Trouble, Shinobi et
Columns : sept variantes de profils/balayage, avec deux budgets de code.
Un dump temporaire des adresses sélectionnées a été ajouté dans l'émetteur
du worktree d'analyse ; il ne change pas le C généré. Les données brutes
restent dans `logs/profile-review/`, les projets dans
`out/profile-review-projects/`, ignorés par Git.

Les budgets demandés de 216 000 et 720 000 octets donnent respectivement
6 000 et 20 000 instructions avec le coût conventionnel GG de 36 octets.
Ce sont des budgets de génération desktop, **pas des tailles de firmware
liées ni une validation du remplissage flash sur console**.

Sans aucun profil, à la limite de 20 000 instructions, le balayage fait bien
fortement croître le nombre d'instructions compilées :

| Jeu | Parcours statique seul, sans balayage | Avec balayage |
|---|---:|---:|
| Ristar | 2 372 | 20 000 |
| Sonic Triple Trouble | 4 240 | 20 000 |
| Shinobi | 754 | 20 000 |
| Columns | 729 | 8 550 |

Avec les deux profils de 24 000 images du premier essai, Ristar, Sonic et
Shinobi remplissent déjà les 6 000 places uniquement avec du code joué.
Le C généré avec ou sans balayage est alors identique octet pour octet.
La sélection dépend toujours du profiling dans ce cas.

À la limite généreuse de 20 000 instructions, les nouvelles positions
réellement jouées grâce aux checkpoints se répartissent ainsi :

| Jeu | Positions nouvelles jouées | Déjà compilées par anciens profils + balayage | Encore absentes de cette sélection |
|---|---:|---:|---:|
| Ristar | 1 252 | 387 | 865 |
| Sonic Triple Trouble | 1 389 | 666 | 723 |
| Shinobi | 14 | 14 | 0 |
| Columns | 3 | 3 | 0 |

Cette dernière colonne ne prouve pas que le balayage serait incapable de
reconnaître ces instructions avec un budget illimité : elle mesure la
sélection effectivement produite sous cette limite. Elle montre que les
checkpoints ne sont pas devenus entièrement redondants.

## Essais hors profil : gain marginal très faible

Comparaison précise native/interpréteur sur Ristar et Sonic, graines 99
et 201, `--start-frame 180`, jusqu'à 24 000 images. Les profils de génération
restent les deux sessions courtes du premier essai, avant/après checkpoints,
avec balayage activé et limite de 6 000 instructions. L'initialisation des
graines reste celle de ce premier essai pour isoler l'effet des checkpoints.

Les compteurs à l'image 3 000 sont soustraits de ceux à l'image 24 000 pour
écarter le début de l'exécution. Les deux graines sont additionnées :

| Jeu | Instructions interprétées, anciens profils | Avec checkpoints | Variation |
|---|---:|---:|---:|
| Ristar | 1 076 001 | 1 077 714 | +0,16 % |
| Sonic Triple Trouble | 14 486 006 | 14 472 113 | −0,10 % |

Les hashes natif/interpréteur correspondent pour chaque variante, graine et
horizon contrôlé (3 000 et 24 000 images). Il s'agit de parties automatiques,
pas de niveaux parcourus et validés visuellement. Aucun gain de temps CPU,
de FPS ou sur matériel n'est déduit de ces compteurs.

Le champ `compiled` du harnais GG compte des entrées dans les régions,
tandis que `interpreted` compte des instructions. Les diviser directement
pour calculer une « part native » mélangerait deux unités différentes.

## Intégration et suite utile

Les deux commits s'appliquent sur le nouveau `main` avec un conflit mécanique
dans les options CLI de `gg_host.c` : conserver `--save-state` et
`--load-state`, puis utiliser `m0_random_seed` pour `--random`.
Cette combinaison a été vérifiée dans le worktree : construction
CMake/Ninja, test unitaire des checkpoints/graines réussi, et deux
explorations Columns identiques de 6 465 images, deux segments et trois
snapshots. La suite complète n'a pas été relancée sur cette combinaison.

Je garderais la correction des graines et le mécanisme de checkpoints,
mais sans présenter ce dernier comme une optimisation majeure démontrée
avec le nouveau balayage. La prochaine mesure utile serait de profiler
les passages lents réels depuis leurs états sauvegardés, puis de comparer
les PC encore interprétés et les lectures de carte, à budget flash lié
identique. Cela cible le coût restant plus directement qu'une multiplication
globale des sessions.
