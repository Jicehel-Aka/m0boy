# Audit du travail de Claude et de la RAM pour le cache SD — 17 octobre 2026

Révision analysée : `326b601` (META 0.10.15 / Pokitto 0.10.11), après
`git pull --ff-only` de `882af67`. gm0/z80 était déjà à jour ; ses deux
fichiers Z80 modifiés localement et son répertoire docs ont été conservés.
Audit et expériences seulement : aucun changement du runtime.

## Appréciation des changements

Les corrections du pad, de la désactivation DMA avant SWRST et du partage du
bus SPI répondent à des défauts matériels documentés dans
`META_PAD_DEBUG_2026-10-15.md`. Le mapper GG fixé à la génération évite une
lecture de la ROM absente de la flash des projets streamés. Les rendus META
restent en flash : la RAM correspondante est donc déjà récupérée ; ne pas
compter une deuxième fois ce gain.

La redistribution GB de 8 secteurs ROM + 2 RAM vers 2 ROM + 8 RAM est
justifiée pour SML2 par les mesures **consignées par Claude sur sa console** :
157 écritures/s auparavant, environ 4/s avec 8 secteurs RAM dans cette exécution,
et environ 42 images/s. Ces chiffres n'ont pas été remesurés ici et ne
justifient pas un optimum universel pour tous les jeux. Le total reste dix
secteurs : ce changement ne crée pas de RAM supplémentaire.

## Défauts et limites observés

### P1 : trop peu de pile dans un firmware GB streamé par défaut

Projet synthétique `tests/test_meta_target_e2e.py:make_rom(1989)`, généré avec
`--target meta --meta-stream on`, configuration par défaut (8 KiB de RAM
cartouche, placée sur SD ; 2 secteurs ROM + 8 RAM). Compilation/édition de
liens ARM GCC 7.2.1 par CMake/Ninja, sources et script de liens générés :

- `.data` : 176 octets ; `.bss` : 31 652 ; `.noinit` : 52.
- `end = 0x20007c88`, `__StackTop = 0x20008000` : **888 octets libres**.
- Ce n'est pas une mesure sur console ni un build PlatformIO officiel : le
  lanceur PlatformIO local ne démarre pas (`ModuleNotFoundError: platformio`).
  Le toolchain ARM et les en-têtes CMSIS installés fonctionnent.
- `meta_main.cpp` place le plancher du dispatcher à `end + 768` : il ne reste
  donc que 120 octets au-dessus de cette réserve, avant même les frames C
  persistantes. Le garde du dispatcher ne protège pas les appels du rendu,
  du menu, de la SD et les interruptions.
- Variante `META_TBLEND=0` liée : `end = 0x200079d8`, soit 1 576 octets
  libres et **688 octets gagnés** ; cela reste sous 2 KiB de pile.
- Relevés `-fstack-usage` (variante sans TBLEND) : `option_menu` 392 o,
  `meta_menu_run` 72 o, `draw_text` 48 o ; `USB_Handler` 120 o, avant
  ses sous-appels et le cadre matériel. Ce sont des tailles de frames,
  pas une mesure de profondeur maximale de la pile.
- Le linker accepte cette situation : aucune réserve minimale de pile
  explicite n'est imposée par cette configuration.

Ne pas transformer les octets libérés en cache avant d'avoir rétabli une
réserve de pile et mesuré sa profondeur réelle. Ajouter un contrôle de pile
à l'édition de liens et une mesure de marge maximale pendant menu, sauvegarde,
streaming et interruptions USB. Les réglages
Pokitto sont dans une branche distincte ; ne pas transposer ces chiffres
à sa RAM.

### P2 : le nouveau LRU ERAM n'actualise pas l'accès à la voie précédente

`gbl_core.c:231` retourne après échange de `eram_mru` et `eram_prev`, sans
mettre à jour `eram_used`/`eram_clock`. Reproduction compilée sur le code
actuel, cache de huit voies vide :

```
secteurs : 0, 3, 0, 6, 2, 1, 9, 4, 8, 10
résultat : 10, 3, 6, 2, 1, 9, 4, 8
```

Le dernier miss évince 0 ; un vrai LRU évince 3, dont le dernier accès
précède le second accès à 0. L'oubli peut provoquer des relectures et des
écritures de secteurs sales évitables. Corriger avant d'agrandir le cache.
Prévoir aussi le vieillissement des entrées très anciennes : le compteur
16 bits reboucle après 65 536 mises à jour, pas après un nombre fixe de secondes.

### Fiabilité SD : défaut préexistant, toujours présent

`eram_writeback` ignore l'échec de `gbl_platform_save_write`, puis efface le
bit dirty ; une éviction réutilise ensuite le tampon. `gbl_save_flush`
efface également `save_pending` même en cas d'échec. Les trois tentatives du
frontend ne garantissent pas une écriture réussie. Ce défaut existait avant
Claude : le cache agrandi ne le corrige pas. Garder dirty/pending en cas
d'échec et traiter l'échec d'éviction avant de réutiliser son tampon.

### Précision sur le « vidage de 2 s »

Le code `meta_save_tick` attend 120 images **après la dernière modification**,
et non 120 images après le précédent vidage. Les écritures continues
repoussent donc ce vidage. Les autres déclencheurs (désactivation de RAM
cartouche, menu/retour au loader, éviction) restent actifs. Le débit mesuré
par Claude décrit son exécution ; il ne prouve pas un vidage périodique
garanti toutes les deux secondes.

## Où récupérer de la RAM

Mesures des symboles RAM des objets ARM, avec les réglages GB streamés
actuels. Elles ne comprennent pas la pile et peuvent différer de quelques
octets après alignement/élimination des sections au lien.

| Piste | Gain | Conséquence et validation nécessaire |
|---|---:|---|
| Deux bandes DMA de 2 lignes au lieu de 4 | **1 280 o** | Tampons 2 560 → 1 280 o ; davantage de transferts/IRQ et moins de recouvrement. Adapter le menu avant emploi. |
| `META_TBLEND=0` | **688 o au lien** | `mode_ram` 672 o et autres données disparaissent ; TILE remplace ce lissage. Delta vérifié sur le firmware synthétique (689 o dans l'objet). |
| `line_kind` et `line_row` en tables constantes flash | **288 o bruts** | Calculées uniquement lors du choix de mode ; pré-calculer les dispositions, garder des pointeurs vers la flash. |
| `line_valid` en bitmap | **126 o bruts** | 144 octets → 18 ; coût de quelques opérations de bits. |
| Mutualiser `smooth_lines` avec `mode_ram` | **80 o bruts** | Les modes SMOOTH/TBLEND sont exclusifs ; vérifier réinitialisation et changements de palette/mode. |
| `GBL_LOOKUP_CACHE=32` au lieu de 64 | **256 o bruts** | Plus de recherches de dispatch ; mesurer les jeux à nombreux appels/retours. |
| `META_AUDIO_DMA=0` | **660 o dans les objets** | L'audio repasse en interruption ; perte CPU à remesurer, ancien commentaire : environ 3 %. |
| `META_USB=0` | **552 o dans l'objet USB** | Perte de l'upload par touch 1200 bauds, du profileur et du diagnostic en direct ; repli de dernier recours. |
| Profileur USB optionnel seul | **132 o bruts** | Conserver CDC et upload, retirer les 33 mots de PCs et leur collecte SysTick. |

**Piège des bandes courtes :** `meta_display_scratch` retourne toute la
surface des bandes. `meta_menu_run:110` refuse moins de huit lignes. Avec
2 × 2 lignes, le menu se ferme aussitôt. Le découper en bandes de quatre
lignes, ou partager un tampon existant dont la durée de vie le permet ;
allouer un nouveau tampon de huit lignes annulerait le gain.

Les deux petites optimisations sans changement d'affichage (tables et
bitmap) rapportent 414 octets avant pointeurs/alignements : seules, elles
ne paient pas un secteur. Un secteur ROM supplémentaire coûte **520 o**
(512 de contenu + tag 32 bits + âge 32 bits), et un secteur ERAM environ
**519 o** (512 + tag 4 + âge 2 + dirty 1), hors alignements.

Les pistes affichage ci-dessus sont spécifiques au GB. Pour GG/SMS, les
8 KiB RAM CPU et 16 KiB VRAM sont l'état de la machine ; les deux lignes de
sortie sont déjà compactes. L'audio DMA et l'USB sont partagés et restent
les réserves les plus visibles. Ne pas supprimer des morceaux de VRAM
sur la seule base d'un jeu : ils sont adressables par le logiciel.

## Priorité proposée

1. Corriger LRU et échecs de write-back, imposer une marge de pile.
2. Passer les bandes GB à deux lignes en adaptant le menu ; mesurer l'attente
   LCD/SD sur console. Le gain de 1 280 o sert d'abord à la pile.
3. Déplacer les dispositions en flash et compacter les booléens ; proposer
   TILE aux jeux qui ont besoin du gain TBLEND, plutôt que le supprimer partout.
4. Avec bandes courtes + TBLEND désactivé + environ 414 o de petites
   optimisations, gain d'environ **2,3 KiB**. Sur le synthétique mesuré,
   cela permettrait 4 secteurs ROM + 8 RAM et environ **2,2 KiB de pile**,
   avant surcoûts du menu/pointeurs/alignements. C'est une projection,
   pas un firmware validé ni une promesse de vitesse.
5. Mesurer par jeu les misses ROM, évictions ERAM sales, temps SD et marge
   de pile. Garder 8 voies RAM pour SML2 tant qu'aucune mesure ne contredit
   le bénéfice observé. Ne pas imposer ce partage aux jeux sans ce besoin.

## Vérifications et artefacts

- Build principal CMake/Ninja réussi.
- Neuf tests ciblés PPU, affichage (normal/streamé/12 bpp), FIFO LCD,
  packing RGB444, APU et FAT : **9/9 réussis**.
- Suite complète : **29/29 réussis**, dont GB, GG/SMS et SG-1000 de bout
  en bout et compilation de la cible Pokitto ; 325,86 s.
- Objets, relevés de symboles, projet synthétique, ELF/map de référence et
  reproduction LRU : `output/ram-audit-2026-10-17/` (ignoré par Git).
- Aucun essai matériel pendant cet audit. Les gains chiffrés sont des
  différences de stockage RAM ; aucun gain de débit SD ou de fps n'est
  revendiqué pour les variantes proposées.

## Mise en œuvre — META 0.10.16 / Pokitto 0.10.12

Les constats précédents décrivent `326b601`, avant ces modifications.
Les trois options sont maintenant disponibles indépendamment :

```
-DMETA_STRIP_LINES=2
-DMETA_TBLEND=0
-DMETA_DISPLAY_COMPACT=1
```

`META_DISPLAY_COMPACT` regroupe les dispositions en flash et le bitmap de
validité ; 0 remet les tableaux RAM. Défauts : bandes DMA de 2 lignes (`META_STRIP_LINES=2`, nombre de lignes), TBLEND
activé, métadonnées compactes activées. Aucun changement d'image par défaut.
Le menu découpe les glyphes pour tenir dans les tampons courts, sans nouvelle
allocation ; il fonctionne même avec une seule ligne de scratch.

Le LRU ERAM utilise une liste circulaire : mise à jour et choix de victime
à coût constant, accès répétés au même secteur inchangés, deux octets de
liens par voie, aucun compteur susceptible de reboucler. Les tests publics
sur le bus le comparent à un modèle indépendant, avec données sales et
150 000 accès pour chacun des caches de 1, 2 et 8 voies.

La banque ROM 0 dans la fenêtre 0000–3FFF était déjà directement en flash.
Une exception a été corrigée : avec MBC5, sa sélection dans la fenêtre
4000–7FFF utilise désormais cette même copie résidente au lieu du cache SD.
Les tests vérifient aussi que ce passage ne chasse pas un secteur ROM déjà
chargé. Cela ne concerne pas le secteur 0 de la RAM de sauvegarde : mutable,
il reste dans le LRU.

Éditions de liens ARM GCC 7.2.1, CMake/Ninja, même cartouche synthétique :

| Variante | RAM statique (.data + .bss + .noinit) | Marge pour pile | .text flash |
|---|---:|---:|---:|
| Trois économies désactivées (4 / TBLEND / tableaux RAM), LRU corrigé | 31 880 o | 888 o | 68 268 o |
| Métadonnées compactes seules, bandes de 4 lignes | 31 464 o | 1 304 o | 70 112 o |
| Défaut final : bandes de 2 lignes, TBLEND et métadonnées compactes | 30 184 o | 2 584 o | 70 112 o |
| Trois économies, 2 secteurs ROM + 8 RAM | 29 496 o | 3 272 o | 69 492 o |
| Trois économies, 4 secteurs ROM + 8 RAM | 30 536 o | 2 232 o | 69 476 o |

Gain total au lien : **2 384 octets**. Métadonnées compactes seules :
**416 octets** dans cette configuration, contre 406 octets théoriques avant
alignements (les deux pointeurs flash coûtent 8 octets). Leurs tables
occupent 2 016 octets de flash ; le delta `.text` net est de +1 844 octets.
La variante complète coûte +1 224 octets `.text` par rapport aux trois
économies désactivées. Le gain RAM sert d'abord à la marge de pile ; le
cache ROM supplémentaire s'obtient avec `-DGBL_STREAM_SECTORS=4`.

Ces budgets dépendent du jeu et du compilateur. Le passage à quatre secteurs
ROM n'est pas imposé automatiquement. Aucun débit SD/fps n'a été mesuré sur
console pour ces variantes. PlatformIO local reste indisponible ; les ELF
ont été liés avec le toolchain ARM et les en-têtes CMSIS installés.

Les 11 tests ciblés affichage/menu/cache passent : pixels indépendamment
vérifiés en mode normal et 12 bpp, chemins compact/ancien, TBLEND actif/inactif,
menu en tranches de 8/4/3/2/1 lignes avec gardes mémoire, LRU et banque 0.
Les 25 autres tests hors GG/SMS passent, dont GB de bout en bout
(streamé, compressé et budgets de pile) et le firmware Pokitto. Le test
GG/SMS a dépassé 300 s pendant le passage parallèle, puis a réussi seul
en 152,30 s, sans changement de code. **37 tests validés au total**.
Artefacts :
`output/ram-cache-options-2026-10-17/` (ignoré par Git).

Le défaut final `META_STRIP_LINES=2` a été revérifié : quatre tests affichage
(normal, streamé, 12 bpp, ancien chemin à quatre lignes) réussis, et lien
ARM réussi. Sur le synthétique, le défaut avec TBLEND conserve 2 584 octets
pour la pile. Les essais sur console restent à faire.
