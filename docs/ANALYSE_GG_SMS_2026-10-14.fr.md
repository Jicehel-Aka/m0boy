# Analyse et pistes d’optimisation GG / SMS — 14 octobre 2026

Les meilleures pistes confirmées sont **le calcul de priorité Game Gear** et
**la construction des masques de tuiles Master System**. Des prototypes isolés
réduisent le coût estimé du parcours synthétique complet de **32,9 % en GG**
et de **5,85 % en SMS**, en combinant plusieurs changements. Ce sont des
estimations Cortex-M0+, pas des mesures sur console ni sur une collection de jeux.

L’analyse initiale porte sur `m0boy`, révision `d65bc30`. `gm0` est le dépôt
voisin de l’émulateur des consoles cibles ; ses sources actuelles n’ont pas de
core GG/SMS direct. Les optimisations retenues ont ensuite été intégrées en
version **0.10.2** (META et Pokitto) : priorité GG conditionnée à la présence de
sprites, masques SMS, comptage des lignes, tables de lignes META et sélection
SMS sans division. Les prototypes et résultats initiaux restent dans
`logs/gg-sms-audit/` ; les projets synthétiques dans `output/gg-sms-audit/`.
Les vérifications de l’implémentation sont dans `logs/gg-sms-implementation/`
et les projets générés dans `output/gg-sms-implementation/`.

## Architecture et points déjà optimisés

GG et SMS partagent `targets/gamegear/` et le générateur Z80 de
`recompiler/src/system/sg1000/`. `GG_SMS` spécialise le système à la compilation.
Le fonctionnement actuel comprend déjà :

- régions natives par banque, appels directs avec vérifications de page ;
- élimination des flags morts et accumulation des cycles/refresh par blocs ;
- VDP rendu à la demande, masques de sprites par bandes de huit lignes ;
- fond en Thumb-1 exécuté en SRAM, décodage des quatre plans par tables ;
- SMS : table de mélanges des paires de couleurs et recomposition des groupes
  sous sprites ou traversant deux palettes ;
- PSG produit par blocs, avec progression simplifiée des canaux muets ;
- cache ROM/SAV de deux secteurs, pages ROM chaudes résidentes en flash.

Un cache global de tuiles décodées ou un framebuffer supplémentaire serait
coûteux en RAM. Dans les images ARM synthétiques avec code natif, deux buffers
de ligne et audio, `.data + .bss` occupent déjà **26 996 octets en GG** et
**28 228 octets en SMS** avec GCC 7.2. Ces nombres concernent le banc d’essai,
sans tous les drivers de la console et sans la pile. Ils ne sont pas un bilan
mémoire de firmware livré.

## Mesures du parcours complet

Cartouche construite depuis `tests/gg_fixture.py` : quatre pages, changements
de banques, IRQ de ligne et de trame, sprites, priorité, défilement, PSG.
La même cartouche est aussi exécutée en mode SMS. Ce second cas exerce le
runtime SMS, mais ne remplace pas une fixture spécifique à ses fonctionnalités.

Les chiffres principaux utilisent **GCC ARM 7.2.1, `-O2`**, présent dans la
chaîne PlatformIO, Thumb Cortex-M0+, `GG_PANEL_SWAP=1`, audio 22 050 Hz et la
politique de sélection/moyenne des lignes META. L’audio META réel utilise
22 048 Hz. Chaque parcours dure 60 trames ; la moyenne ci-dessous porte sur
les phases entre les rappels de trame 5 et 59, après l’initialisation.

Le modèle reprend `classify()` de `targets/meta/tools/m0bench/m0_estimate.py` :
charges/écritures, branchements, appels et attentes flash modélisées par
instruction. L’exécution est faite dans Unicorn. Le hachage de vérification
est exclu du coût ; les périphériques LCD/SD, les interruptions audio du
Pokitto et les contentions de bus ne sont pas simulés. Les branchements
conditionnels ont un coût moyen dans ce modèle. Les valeurs servent à comparer
les variantes, pas à annoncer une vitesse matérielle.

| Système / changement isolé | Cycles estimés / trame | Réduction du coût |
|---|---:|---:|
| GG, référence | 1 140 658 | — |
| GG, priorité calculée seulement si des sprites sont présents sur la ligne | 801 154 | **29,76 %** |
| GG, politique de lignes précalculée | 1 109 513 | **2,73 %** |
| GG, comptage arithmétique des lignes | 1 135 978 | 0,41 % |
| GG, trois changements combinés | **765 329** | **32,90 %** |
| SMS, référence | 1 360 082 | — |
| SMS, masques construits algébriquement et décalés progressivement | 1 299 507 | **4,45 %** |
| SMS, sélection des lignes sans division | 1 345 776 | **1,05 %** |
| SMS, masques + sélection + comptage des lignes | **1 280 521** | **5,85 %** |

Toutes ces variantes conservent les hashes cumulés de pixels et d’audio,
ainsi que le hash d’état du banc ARM. Leur occupation statique de RAM reste
identique à celle de la référence du même système. Les réductions sont
relatives au coût, pas des gains de fréquence d’images de même pourcentage.

Les tendances sont aussi vérifiées avec GCC 16.2 : priorité GG −28,61 %,
masques SMS −6,32 %, sélection SMS −2,03 %. Le compilateur influe donc
sensiblement sur les chiffres, notamment pour les divisions constantes.

## 1. Priorité GG : première optimisation à intégrer

Source : `targets/gamegear/runtime/gg_vdp.c:219`, `:238`, `:278`.

Après le fond en assembleur, `background()` redécode les tuiles prioritaires
en C et extrait chaque pixel opaque pour remplir `bg_prio`. Cette seconde
passe est effectuée même si `line_sprites()` a retourné zéro. Le masque ne
sert ensuite qu’à empêcher les sprites de couvrir ces pixels.

Le prototype passe un masque nul à `background()` lorsqu’il n’y a aucun
sprite sur la ligne et quitte après le rendu du fond, avant cette seconde
passe. Le fond, la recherche des sprites, le débordement et les collisions
gardent leur fonctionnement. Aucun cache ni état supplémentaire.

Dans le microbanc GCC 16.2, sur 144 lignes GG :

| Scène | Référence | Prototype | Réduction |
|---|---:|---:|---:|
| Fond sans priorité, sans sprite | 386 313 | 386 745 | régression de 0,11 % |
| Fond prioritaire, sans sprite | 1 244 895 | 413 025 | **66,82 %** |
| Fond prioritaire, sprites répartis sur certaines lignes | 1 503 843 | 943 917 | **37,23 %** |

Le cas prioritaire sans sprites est volontairement défavorable à la référence.
Le gain réel dépendra de l’usage des tuiles prioritaires et de la présence
des sprites. Une seconde étape possible serait de calculer seulement les
pixels de priorité qui croisent des sprites visibles ; elle n’a pas été testée.

## 2. Masques SMS : simplification sans mémoire supplémentaire

Source : `targets/gamegear/runtime/gg_vdp.c:338`, `:654`.

`sms_compose()` construit les tuiles à redécoder en parcourant les 32 groupes
et en effectuant des décalages 64 bits variables. Pourtant, le masque est
directement exprimable :

```c
uint64_t tiles = groups;
if (fine) tiles |= tiles << 1;
```

Chaque groupe couvre sa tuile et, avec un défilement fin, la suivante.
Le bit 32 doit être conservé : un simple masque 32 bits perdrait cette tuile.
Dans `sms_bg_indices()`, lire `tiles & 1`, puis décaler `tiles` d’un bit par
itération évite également le décalage variable `tiles >> k`.

Ce changement concerne le chemin de recomposition sous sprites et aux
frontières de palettes. Il n’apporte pas de gain lorsque toute la ligne suit
le chemin rapide. Le gain est de 4,45 % sur le parcours complet GCC 7.2,
et de 2,54 % sur la scène du microbanc GCC 16.2 avec sprites localisés.

## 3. Politique de lignes : supprimer des divisions répétées

Source : `targets/gamegear/platform/gg_main.cpp:62`, `:67`, `:88`.

SMS : les cinq lignes retenues dans chaque groupe de huit sont les résidus
`0, 2, 4, 5, 7`. Le prédicat actuel contenant `/ 5` est équivalent, sur les
192 lignes, à :

```c
return (0xb5u >> (y & 7)) & 1u;
```

GG sur META : `draw_line()` évalue plusieurs fois la conversion `/ 9` pour
choisir une ligne simple ou une paire à moyenner. Une table en flash de
144 décisions remplace ces calculs. La conversion de numéro de ligne pour
l’envoi LCD peut aussi être précalculée dans une seconde table de 144 octets.
Le gain mesuré ci-dessus ne couvre que la table des décisions.

La table GG concerne la politique META ; le Pokitto a une autre conversion
verticale. Le prédicat SMS est commun aux deux frontends. Aucun chiffre
de performance Pokitto n’a été mesuré dans cet audit.

## 4. Compteur de lignes : gain local fort, gain global modeste

Source : `targets/gamegear/runtime/gg_vdp.c:475`.

`gg_line_counter_catchup()` parcourt chaque ligne écoulée, même en l’absence
d’interruption de ligne observable. On peut calculer le nombre de lignes
atteintes, avancer le compteur jusqu’au premier underflow, puis calculer
la position dans la période `R10 + 1`. Les lignes après 192 rechargent R10.

Le prototype conserve le flag d’IRQ, les modifications de R10 entre appels,
les deltas négatifs et le débordement de `frame_start`. Deux millions
d’états pseudoaléatoires donnent les mêmes résultats que la boucle d’origine.

Microbanc GCC 16.2 : −96,22 % pour un rattrapage de trame entière sans
underflow actif, −53,08 % avec rattrapages espacés de huit lignes et R10=48,
−8,06 % avec R10=0. Sur le parcours complet GG, le gain est seulement
0,41 % avec GCC 7.2. À traiter après le rendu ; surveiller le coût de la
division variable lors des underflows.

## Pistes testées mais non retenues sous leur forme actuelle

**Éviter le double filtrage SMS sous sprites.** Le prototype transmet au
rendu assembleur les groupes déjà destinés à la recomposition. Il conserve
les pixels, mais ajoute un test par groupe. Avec GCC 16.2, il ralentit les
scènes sans sprites d’environ 3 % et la scène avec sprites de 0,49 %.
Une stratégie différente, par plages ou avec un chemin spécialisé, serait
nécessaire pour rendre cette idée rentable.

**Mélanges SMS mis à jour par couleur sale.** Le prototype ignore les
écritures CRAM identiques et ne recalcule que les paires touchées. Sur les
scènes où une seule couleur est écrite par trame, la réduction mesurée est
de 0,25 à 1,01 %, avec une régression de 0,26 à 0,42 % ailleurs et huit
octets statiques de plus. L’invalidation et le parcours du masque coûtent
assez cher pour que cette version soit secondaire. Les effets de palette
fréquents en cours de trame pourraient changer ce bilan ; ils ne sont
pas représentés par ce microbanc.

## CPU, dispatch et SD : investigations suivantes

Le parcours synthétique attend surtout en `HALT`. Il renseigne bien sur le
rendu, mais peu sur les jeux faisant beaucoup de calculs Z80 ou de lectures SD.
Les pistes suivantes viennent de l’inspection du code ; aucun gain n’est chiffré.

1. **Cache des recherches négatives du dispatch.** Le générateur
   (`emitter.cpp:981–985`) mémorise les entrées compilées, mais pas l’absence
   d’entrée. Une boucle interprétée en ROM peut refaire la recherche binaire
   à chaque retour au dispatcher. Une clé `loc + 1` permet de distinguer une
   case vide d’un résultat nul valide, avec les tableaux existants. Il faut
   tester les collisions de cache, les changements de banque et les petits
   budgets de code.
2. **Boucles d’attente Z80.** `sg_halt()` saute déjà jusqu’au prochain événement,
   mais le générateur n’a pas les reconnaisseurs de boucles d’attente du
   backend Game Boy. Les boucles sur le compteur V, les flags RAM d’IRQ et
   les délais DJNZ sont candidates. Préserver les lectures observables,
   flags, refresh R, mapper et instant de prise d’interruption est indispensable.
3. **Accès mémoire spécialisés.** `sg_gen.h` utilise volontairement des helpers
   `noinline` pour économiser la flash. Les accès dynamiques à RAM, PUSH/POP
   et les copies de blocs pourraient avoir des chemins rapides, à condition
   de garder le repli pour les miroirs, limites de zones et registres mapper.
   Toute expansion doit être évaluée sur son effet sur le budget de code
   et donc sur le nombre d’instructions restant interprétées.
4. **Profil de misses SD.** Le placement ROM (`generator.cpp:161–173`) additionne
   les nombres de trames où chaque secteur a été lu et garde des pages
   entières de 16 Kio. Ce score n’est pas un nombre de misses du cache.
   Il faut enregistrer la séquence des secteurs et les évictions ROM/SAV
   avant d’arbitrer un placement plus fin ou un troisième secteur de cache.
   Un secteur supplémentaire coûte au moins 512 octets de RAM.
5. **Progression du rendu et LCD.** Le runtime prévoit `GG_RENDER_STEP`, mais
   aucun réglage par défaut n’a été trouvé dans les sources de génération.
   Le rendu peut donc rester en rafales à VBlank ou aux accès VDP.
   Mesurer l’attente DMA/SD et le frameskip dans gm0 permettrait d’évaluer
   ce réglage ; multiplier les synchronisations peut également coûter plus cher.

Sur la fixture GCC 7.2, le fond GG et la seconde passe C de rendu représentent
environ 84 % du coût total. En SMS, `gg_sms_bg`, `sms_compose` et `sms_render`
représentent environ 89 %. Le PSG produit environ 3–4 %. Le rendu est donc
la priorité sur ce workload ; cette répartition ne vaut pas pour tous les jeux.

## Vérifications et limites avant intégration

- Build CMake/Ninja Release réussi ; tests `z80_execution_reference`,
  `gg_native_end_to_end` et `source_system_profiles` réussis.
- GG et SMS synthétiques, 60 trames avec entrées identiques : le code natif
  précis reproduit l’interpréteur, `run_hash` **23785189** en GG et
  **9fce0342** en SMS ; fichiers pixels et WAV identiques.
- Prototypes GG priorité, GG compteur, SMS compteur et SMS masques :
  mêmes résultats précis natifs, pixels et audio que la référence.
- Bancs ARM complets, variantes isolées et combinées : hashes cumulés
  pixels/audio et état identiques avec GCC 7.2 et, pour les variantes
  exécutées avec lui, GCC 16.2.
- `GG_SELFTEST` exécuté en image ARM autonome sous Unicorn, référence et
  prototype priorité, `GG_PANEL_SWAP` 0 et 1 : **zéro ligne différente**.
  Cela vérifie les chemins assembleur/C ; ce n’est pas un firmware de
  console exécuté dans gm0.
- Comptage arithmétique : **2 000 000 états** comparés à la référence.
- Pas de collection commerciale GG/SMS locale : les 225 cartouches citées
  dans la documentation antérieure n’ont pas été revérifiées ici.
- Lors de l’analyse initiale, PlatformIO échouait avec
  `ModuleNotFoundError: platformio`. L’implémentation utilise son environnement
  existant via `PYTHONPATH=/home/m/.platformio/penv/lib/python3.13/site-packages`
  (les paquets sont dans le dossier Python 3.13, l’exécutable est désormais
  Python 3.14). Les builds META de l’implémentation ont ensuite réussi.

Pour intégrer les pistes retenues : commencer par priorité GG, masques SMS
et sélection des lignes ; garder des changements séparés et mesurer ensuite
leurs combinaisons. Compléter les tests SMS pour PAUSE/NMI, CRAM, masquage
gauche et IRQ, ainsi que les cas Codemasters, ROM en streaming et SAV.
Construire les firmwares des deux consoles, vérifier RAM/flash/pile et lancer
`GG_SELFTEST` dans gm0 pour les deux ordres de dalle. Une modification de
comportement des firmwares doit suivre les règles de version d’`AGENTS.md`.

Les profils détaillés sont dans `logs/gg-sms-audit/*.json`, les preuves natives
dans `variant-native-validation.txt`, et les selftests dans
`selftest-results.txt`. `run_arm.py` utilise les ELF conservés localement ;
les dépendances Capstone de l’audit sont dans son sous-dossier `python-deps/`.
Exemple de relecture :

```sh
PYTHONPATH=logs/gg-sms-audit/python-deps \
  python3 logs/gg-sms-audit/run_arm.py \
  logs/gg-sms-audit/gcc7-combined-gg.elf --full
```

## Validation de l’implémentation 0.10.2

La collection ajoutée localement contient **335 ROMs GG et 398 ROMs SMS**,
y compris les fichiers SMS placés dans le dossier GG. **733 sur 733** passent
le contrôle suivant, sur **360 trames par ROM**, avec `--random 42` et
`--start-frame 30` :

1. Interpréteur avant changement = interpréteur après changement : même
   état final (`run_hash`, cycles, PC et banques), capture finale et fichier WAV.
2. Code natif précis après changement = interpréteur après changement :
   mêmes résultats, avec du code natif effectivement exécuté dans chaque cas.

Le budget de génération est de 65 536 octets : le code non compilé reste
interprété. Pour accélérer cette grande vérification, la plupart des projets
réutilisent les bibliothèques du runtime générique de `gg_host` et compilent
leurs régions natives avec Clang sans optimisation. Les premiers projets ont
été construits séparément avec GCC. Ce contrôle valide l’équivalence des
parcours exercés ; il ne mesure ni la vitesse des firmwares ni la jouabilité
de tous les jeux, et n’explore pas toutes leurs situations.

Le test `tests/test_gg_e2e.py` couvre désormais également le SMS, PAUSE/NMI,
les builds précis, le repli et les builds par défaut, pour les trois cibles.
`tests/test_gg_runtime.c` vérifie les deux millions d’états de compteur,
le rendu GG/SMS contre les indices de couleurs et un filtre par composante,
ainsi que toutes les lignes des politiques d’affichage.
Les quatre tests CTest ciblés passent (`z80_execution_reference`,
`gg_sms_runtime_reference`, `gg_native_end_to_end`, `source_system_profiles`).

Les firmwares `GG_SELFTEST` META et Pokitto, chacun avec `GG_PANEL_SWAP` à
0 puis 1, donnent **`gg_selftest_result = 1`** dans gm0. Le résultat est lu
dans la RAM émulée à l’adresse du symbole du firmware.

Quatre jeux ont en outre été construits avec GCC ARM 7.2 et démarrés dans gm0
sur les deux cibles, sans HardFault, avec lecture de la carte pour les projets
en streaming :

| Jeu | Cible | Sections RAM, octets | Firmware binaire, octets |
|---|---|---:|---:|
| Columns | META | 27 680 | 176 440 |
| Columns | Pokitto | 28 120 | 176 672 |
| Aladdin | META | 28 848 | 234 848 |
| Aladdin | Pokitto | 29 312 | 232 292 |
| Alex Kidd in Miracle World | META | 30 080 | 230 596 |
| Alex Kidd in Miracle World | Pokitto | 30 544 | 222 892 |
| Shinobi | META | 30 080 | 229 764 |
| Shinobi | Pokitto | 30 544 | 222 652 |

Les sections RAM n’incluent pas la pile ; le Pokitto répartit certaines
données entre ses banques. Aladdin nécessitait un ajustement du budget flash,
comme le prévoit `compile.sh` : 45 octets estimés par instruction sur META,
42 sur Pokitto. Les versions de production ont été portées à **0.10.2** dans
les deux fichiers VERSION et dans les tableaux des README.

Les résultats par ROM, couvertures, logs, captures et fichiers audio restent
dans `output/gg-sms-implementation/collection/`, hors Git. Les résultats des
builds et démarrages sont dans `logs/gg-sms-implementation/`. Le contrôle
de collection est relançable avec :

```sh
python3 tests/test_gg_collection.py --rom-dir roms \
  --recompiler build/bin/m0recomp --host build/bin/gg_host \
  --baseline-host logs/gg-sms-implementation/gg_host_before \
  --output output/gg-sms-implementation/collection \
  --frames 360 --jobs 4 --runtime-libs build/lib --compiler clang
```

Omettre `--runtime-libs` reconstruit le runtime spécialisé de chaque projet.
`--resume` est réservé à une interruption avec les mêmes sources et paramètres.

## Suite : version 0.10.3, mesurée sur de vrais jeux dans gm0

Les chiffres ci-dessus viennent d'une cartouche synthétique. Pour la suite,
cinq jeux ont été mesurés en firmware Pokitto complet dans gm0 (cœur
Cortex-M0+, LCD, carte SD et timer audio émulés) : Sonic the
Hedgehog, Columns, Shinobi, Disney's Aladdin (Game Gear) et Alex Kidd in
Miracle World (Master System). Firmware de banc : touches scriptées par
trame GG dans `frame()`, ni attente ni saut d'image, arrêt à la trame 900 ;
le profil `EMU_PROF` donne les cycles jusque-là, sans la boucle finale ni le
hachage. Le firmware écrit dans l'EEPROM émulée un hash de l'état final
(registres, cycles, RAM, VRAM, CRAM, VDP, tampon audio) et un hash cumulé de
toutes les lignes envoyées à l'écran : il est identique pour chaque jeu
entre 0.10.2 et chaque étape ci-dessous.

| Jeu | 0.10.2, cycles / trame | 0.10.3 | Écart |
|---|---:|---:|---:|
| Sonic the Hedgehog | 1 337 396 | 1 034 854 | −22,6 % |
| Columns | 1 143 253 | 992 832 | −13,2 % |
| Shinobi | 1 378 151 | 1 190 342 | −13,6 % |
| Disney's Aladdin | 1 192 090 | 970 830 | −18,6 % |
| Alex Kidd in Miracle World | 1 363 208 | 1 221 613 | −10,4 % |

Ce sont des mesures d'émulateur, pas de console : gm0 modélise le cœur et
les attentes de flash, mais le SSP de la carte SD y est instantané (une
lecture de secteur coûte plus cher sur une vraie Pokitto) et le LCD n'y a
pas de délai propre. Les projets de banc ont été générés avec 10 s de
parties automatiques par ROM. Ce n'est pas une fréquence d'images : au
72 MHz que règle le firmware, 1,2 million de cycles font une trame à 60 Hz.

Par étape (même banc, mêmes hashes) :

- **Tuiles répétées (asm).** Fond GG : une tuile de même entrée de nom que
  la précédente recopie ses 8 pixels ; `gg_bg_tiles` −19 à −33 %, −4,2 à
  −7,0 % au total. Fond SMS : champs réutilisés pour une tuile répétée, et
  groupe de 5 pixels recopié s'il égale le précédent ; −5,9 % (Alex Kidd).
  `GG_SELFTEST` a désormais des tours à table de noms répétitive ; cinq
  mutations de ces chemins y sont détectées.
- **Cache SD de 4 secteurs (Pokitto, Game Gear).** La RAM USB de 2 Kio ne
  contenait que les 2 secteurs du cache ; elle en tient 4, sans RAM
  principale. Sonic en jeu : 532 secteurs lus au lieu de 15 123, −12,4 %.
  Le Master System y garde ses tables de paires (2 secteurs) ; le META,
  dont la RAM principale est juste, garde 2 secteurs.
- **Interruption audio Thumb-1** (celle du Game Boy) pour GG/SMS sur
  Pokitto : −1,5 à −1,8 %.
- **Table 144 → 176 lignes** sur Pokitto au lieu de deux divisions
  logicielles par ligne : −1,4 à −1,6 % en GG.
- **Priorité GG** limitée aux tuiles sous un sprite (−0,1 à −1,2 %),
  **sprites entiers** sans tests par pixel (−0,5 à −1,8 %). Le test hôte ne
  testait les sprites que zoomés : corrigé.
- **OUTI/OUTD compilés** par des helpers (`sg_outi`, `sg_outd`) au lieu de
  `sg_op` : Columns −3,0 %, Shinobi −3,2 %, Alex Kidd −2,1 %, Sonic et
  Aladdin sans effet notable ; moins de flash (−3,5 Kio sur Shinobi).
- **OUTI/OUTD en RAM** sans le décodage générique de l'interpréteur :
  Aladdin y exécute 178 000 de ses 181 000 instructions interprétées ;
  −6,3 %. `test_gg_runtime` compare 2 000 suites aléatoires à `sg_step`.
- **Écran coupé** rempli par mots, groupes SMS recomposés sous les seuls
  pixels opaques des sprites : −0,4 à −2,6 %.

Vérifications : CTest complet ; e2e GG et SMS (la cartouche synthétique
exécute OUTI/OUTD, flags lus et non lus) ; `GG_SELFTEST` Pokitto (EEPROM)
et META (couleur de l'écran) à 1 pour `GG_PANEL_SWAP` 0 et 1 ; builds
META de Sonic, Aladdin et Alex Kidd, démarrés dans gm0, capture de Sonic
identique octet pour octet à celle de 0.10.2. Le code asm du fond,
exécuté en SRAM, prend 64 octets de RAM de plus sur META (Aladdin 28 912
octets statiques au lieu de 28 848). Les 733 ROMs du contrôle de
collection n'ont pas été rejouées pour ces changements, et rien n'a été
mesuré sur une console.

Restent visibles au profil : l'envoi des lignes au LCD du Pokitto (24 à
30 %, `pokitto_push200`, borné par les GPIO), dont l'adressage par ligne
(`at`, environ 1,7 %) pourrait céder à une fenêtre GRAM fixée par trame,
non tentée sans essai sur la dalle réelle ; le fond (15 à 33 %) ; le code
natif (8 à 11 %) ; l'interpréteur pour le code hors couverture (jusqu'à
9 % sur Shinobi, environ 400 cycles par instruction Z80) ; le PSG (3 à
5 %).

Suite, sur un jeu qui saute des images (Ristar) : profil, secteurs de ROM gardés
en flash et adressage du LCD, version 0.10.4 :
[GG_RISTAR_VITESSE_2026-10-14.fr.md](GG_RISTAR_VITESSE_2026-10-14.fr.md).
