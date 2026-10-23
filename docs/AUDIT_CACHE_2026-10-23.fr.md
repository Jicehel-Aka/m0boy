# Audit du cache et des derniers commits — 23 octobre 2026

Révision analysée : `10f44cc`, après pull fast-forward depuis `04f4aca`.
Analyse de code, reproductions synthétiques et tests hôte. Les mesures de
console citées ci-dessous proviennent des commits et du rapport du 22 octobre ;
elles n'ont pas été refaites pendant cet audit. Aucun runtime modifié.

## Conclusion

Le cache unifié à blocs de 64 octets est un choix bien étayé pour les scènes
META étudiées. Les chemins rapides et les copies spécialisées apportent un
gain réel. En revanche, ni le partage pile/cache, ni le placement en flash
ne sont optimaux au sens global. Le pipeline automatique ne reproduit pas
encore toutes les configurations des firmwares. Deux défauts de sûreté du
cache, préexistants au pull, sont reproduits : perte de sauvegarde sur échec
SD et lecture hors tableau lors du prêt intégral au menu.

## Mise à jour et commits

- m0boy : `04f4aca` → `10f44cc`, sept commits, modifications locales conservées.
- gm0 : pull sur la branche active `z80`, `7f68895`, déjà à jour avec
  `origin/z80`. Les références distantes sont actualisées : `origin/main`
  est maintenant `a48c411`. La branche locale `main` diverge (28 commits
  propres, 39 distants) et deux fichiers Z80 sont modifiés. Aucune fusion
  ni bascule effectuée pour préserver ce travail.
- `ac30e60` : corrige le refus systématique d'entrer dans le code SMS natif
  faute de marge de pile ; utilise la flash résiduelle pour plus de ROM.
  Le gain SMS ne doit donc pas être attribué entièrement au cache.
- `06dd461` : réécrit le fond SMS ; −39 % de cycles sur la scène documentée
  dans gm0, avec comparaisons d'image et selftest. C'est une estimation
  d'émulateur, pas une mesure matérielle.
- `5a3cefa` : cache adaptatif, défaut META à 64 octets, correction du
  profilage de RAM cartouche GB, pile ESPboy de 12 Kio. La correction des
  profils peut elle-même améliorer fortement la couverture du code.
- `7a97b8e`, `ac95232` : états Sega puis PICO-8 ; élargissent les cas où
  le menu et son prêt de mémoire doivent fonctionner.
- `a321197` : documentation ; `10f44cc` : traces v2, simulation, profilage
  commun, arbitrage de stockage et copies Thumb-1.

Le gm0 publié ne contient pas les options `--play-guest`, `--until-guest`,
`--record-input`, `--dump-ram` invoquées par les nouveaux outils. Leur absence
est vérifiée dans `gm0.c` de la branche active et de `origin/main`. Le pipeline
de mesure du rapport du 22 octobre dépend donc d'une version de gm0 qui
n'est pas disponible dans ces révisions. Actualiser les références ne suffit
pas à le rendre reproductible ici.

## Organisation et répartition

Il faut distinguer la granularité du stockage (secteurs SD/LZSS de 512 octets,
blocs de flash éventuellement plus grands) de celle de résidence en RAM
(64 octets sur les configurations META concernées).

Une lecture trouve successivement : la ROM directement mappée ou le pool
brut en flash ; le dernier bloc RAM ; le secteur de transit de 512 octets ;
le tableau de tags ; enfin une victime CLOCK et un chargement SD/LZSS.
Le secteur de transit sert directement les voisins du bloc demandé, sans
les installer tous dans le cache. Le cache est donc aussi un cache de
fragments dispersés, avec une réserve de localité séquentielle de 512 octets.

ROM et RAM cartouche partagent les voies. Les écritures de sauvegarde sont
différées ; un writeback de sous-bloc rassemble les autres blocs sales du
même secteur et fait une lecture-modification-écriture si nécessaire.
CLOCK protège le dernier bloc ROM et le dernier bloc de RAM cartouche.
Cette politique adapte les occupants, mais ne tient pas compte du prix
différent d'une victime sale, SD ou LZSS.

| Configuration | Données RAM du cache | Autres coûts à compter |
|---|---|---|
| META GG/SMS dynamique | fixe 768 o = 12 × 64 ; extension selon RAM libre ; plafond 4096 o | transit 512 o ; tags/références/saleté au plafond : 384 o ; contrôle |
| META GB unifié, valeurs par défaut ROM+RAM sur SD | cible 3904 o = 61 × 64 ; fixe 1472 o ; plafond 5952 o = 93 voies | transit 512 o ; tables ERAM 1024 o sur ARM ; métadonnées au plafond 558 o ; contrôle |
| Pokitto GG | 4 × 512 = 2048 o, banque USB, fixe | métadonnées en plus ; pas de transit supplémentaire |
| Pokitto SMS | défaut 2 × 512 ; partage de la banque USB avec les tables SMS | ne pas lui appliquer automatiquement le modèle GG à 4 voies |
| ESPboy Sega streamé | 6 × 512 = 3072 o, fixe | source en flash externe, sans SD ; coûts différents de la META |

Les valeurs GB supposent les defines usuels, notamment audio par interruption.
Le placement réel et les overrides du jeu priment : il faut lire l'ELF/map.
Un maximum de voies réserve ses métadonnées même lorsque le cache est petit.
Pour le GB donné en exemple, payload + transit + tables + métadonnées font
déjà 5998 octets à la cible, hors contrôle ; « cache 3904 octets » ne décrit
donc pas son coût mémoire complet.

Ordre mémoire dynamique : données fixes → `.cachepool` → extensions du
cache → RAM libre → réserve des appels non gardés/IRQ → limite → pile C.
Le menu emprunte la partie fixe après flush ; le cache est invalidé.

## Optimalité : ce qui est établi et ce qui reste heuristique

**64 octets.** Le rapport `PROFILING_STOCKAGE_2026-10-22.fr.md` compare, à
payload comparable, 14 × 64 (9351 cycles/image estimés), 28 × 32 (9687),
56 × 16 (10276), 7 × 128 (10463), 2 × 512 (15742). Cela soutient 64 sur
Ristar ; la comparaison doit aussi compter les métadonnées et les tables,
particulièrement sur GB. Ce n'est pas une preuve pour tous les jeux.

**Unification.** Elle évite une partition rigide code/données/RAM lorsque
leurs besoins changent. Les variantes séparées essayées perdent sur Ristar.
Cela ne prouve pas qu'une autre politique de remplacement ne pourrait pas
éviter la pollution du cache par une longue lecture ponctuelle.

**Pile.** Un refus rend un bloc par image ; la reprise attend 8 × 30 =
240 images calmes, soit environ 4 s à 60 Hz, et 256 octets de marge.
Le message du commit annonçant environ 8 s ne correspond pas aux constantes.
Le cache ne descend jamais sous sa taille après boot, et non simplement sous
sa partie fixe. La pile ne peut donc pas récupérer cette allocation initiale,
même si plus de code natif serait alors exécutable. Réciproquement, un seul
chargement dans chaque fenêtre suffit à demander une croissance : aucune
mesure de baisse des défauts ou des cycles n'est exigée. L'hystérésis évite
les oscillations rapides ; elle n'optimise pas le coût total pile/cache.
La borne initiale de GG dépend d'une garde 2000/2400 octets, pas d'une
observation du jeu en cours.

**Flash.** `m0romsim::choose` trie gain/coût, dépense par tours et resimule
au plus huit fois. Il peut promouvoir du compressé en brut, mais ne recherche
pas les échanges généraux de blocs déjà choisis. Les interactions du cache,
les tailles et les coûts fixes rendent ce choix non additif : il s'agit
d'une heuristique gloutonne, sans borne d'écart à l'optimum.

Le générateur ne consomme pas directement le placement simulé : `rom.rank`
est calculé avec trois fois le budget, puis ses gains sont redistribués entre
secteurs (GG) ou chunks de 4 Kio (GB), codec et taille étant relus sans
imposer le choix. GG refait ensuite l'arbitrage secteurs/code. Des économies
de chemins d'accès et de décodage ramenées à un équivalent lecture SD sont
ainsi réinterprétées comme des taux d'accès : les scores restent utiles,
mais le résultat final n'est pas le plan évalué par le simulateur.

**Résultats matériels rapportés, Ristar, même partie de 5500 images :**
133,37 → 130,82 s (−1,9 %), images >25 ms 1657 → 1149 (−30,7 %),
décodages 2119 → 21. Avec les mêmes nouveaux coûts, le plan par fréquence
fait 130,84 s : aucune supériorité mesurable du plan simulé sur cette scène.
Le gain établi vient des chemins d'accès, des copies et de l'arbitrage avec
les nouveaux coûts. Ces mesures utilisaient un cache fixe de 12 voies :
elles ne valident pas l'optimalité du partage dynamique.

## Défauts et limites constatés

1. **P1 — prêt intégral au menu, hors limites, préexistant.**
   `m0_cache.inc:275` cherche une voie non prêtée sans vérifier la borne.
   GG définit `M0C_LEND_KEEP=0`. Sur cache fixe, toutes les voies sont
   prêtées ; `cache_lend` lit après les tags, puis les indices du dernier
   bloc peuvent rester invalides après `cache_unlend`. Reproduction sous
   AddressSanitizer : `global-buffer-overflow` dans `cache_drop`.
   Cas concerné notamment : Sega fixe, donc Pokitto/ESPboy et META sans
   extension active. Le repro prouve le hors-tableau, pas un crash sur
   chaque firmware. Il faut traiter explicitement le cache entièrement
   suspendu et réinitialiser ses indices avant reprise.

2. **P1 — sauvegarde perdue si le writeback échoue, préexistant.**
   `m0_cache.inc:102-109` appelle `cache_writeback`, puis efface tag et
   dirty sans vérifier le succès. Le nouveau `cache_shrink` fait pourtant
   ce contrôle. Le prêt au menu invalide également après un flush refusé.
   Repro : trois voies, blocs de 64 octets, écritures SD refusées ; écrire
   0x42 à l'offset 0, puis 64 et 128 ; relire 0 donne 0x00, valeur de la
   carte synthétique. Le bit pending ne restaure pas les données perdues.
   Une victime sale doit rester résidente tant qu'elle n'est pas écrite,
   avec une stratégie explicite si aucune victime sûre n'est disponible.

3. **P2 — répétitions d'écritures mal simulées.**
   `m0romsim.cpp:592-596` compte les répétitions RAM sans rappeler
   `cart_access` : ni date du dernier changement, ni dirty/pending après
   flush. Le format permet les répétitions de part et d'autre d'une fin
   d'image. Un changement répété chaque image peut donc déclencher à tort
   le flush des 120 images, puis ne plus être compté comme sale. Repro
   synthétique fourni avec cet audit ; les accès alternés dans le même
   bloc constituent le témoin sans compression en répétitions : sur 131
   images avec changement à chaque image, le témoin donne 0 écriture SD,
   les répétitions en donnent 1, alors que le résultat attendu est 0.

4. **P2 — modèle automatique différent du firmware.**
   `m0profile.py:648-680` n'active jamais `--cart-on-card` : les traces de
   RAM cartouche ne concurrencent pas la ROM. Sans calibration, GB hérite
   de 12 voies GG et non du pool GB ; Pokitto est modélisé à quatre voies
   même pour SMS, puis le cas GB force des blocs de 64, tandis que son
   profil Pokitto reste distinct. Les coûts `lean` sont ceux de GG/META.
   Le simulateur a la capacité d'étudier ces variantes, mais la commande
   automatique ne les paramètre pas complètement.

5. **P2 — plan dérivé partagé entre cibles, invalidation incomplète.**
   `rom.plan`, `rom.rank`, `storage.json` sont uniques dans le dossier
   commun (`m0profile.py:706-713`). `compile.sh:929` et `:1381` peuvent
   réutiliser le rang META en Pokitto si les couvertures n'ont pas changé.
   Ils ne vérifient pas une identité du runtime, du simulateur, de la cible,
   du cache ou du code compilé. GB ne surveille même pas la calibration.
   Partager les sessions est pertinent ; ces produits dépendent de la cible
   et doivent être nommés et invalidés en conséquence.

6. **P2 — trafic de la génération précédente.**
   Pour Sega, le projet précédent est utilisé si disponible ; après
   arbitrage, le code compilé change et donc les fetches interprétés aussi.
   Aucune convergence finale n'est imposée. GB exclut explicitement
   `project_host` dans `ensure_traces`, et compile.sh ne lui passe pas
   `--project` : il conserve le trafic du harnais générique avec filtrage
   éventuel, dont la divergence de minutage est connue et documentée.

## Suite recommandée

Corriger d'abord les deux défauts de sûreté et ajouter les cas d'échec SD
et de prêt intégral aux tests. Puis corriger les répétitions RAM, dériver
les paramètres du simulateur de la configuration effective et distinguer
les produits de stockage par cible et empreinte des entrées.

Pour juger la répartition, relever par scène les voies actives, refus de
pile, exécutions natives/interprétées, classes de hits, lectures/écritures
SD, décodages et temps par image. Comparer à RAM totale égale des tailles
fixes et dynamiques, en conservant les mêmes entrées. Tester quelques
échanges locaux de secteurs et variantes de remplacement avant de changer
les codecs. Une validation sur le firmware final et des sessions gardées
à l'écart est nécessaire ; une moyenne de cycles ne suffit pas à évaluer
les pauses les plus visibles.

## Vérifications locales

Reproductions et journaux : `out/cache-audit-2026-10-23/` (ignoré).
`write_failure.c` reproduit la perte de sauvegarde ; `lend_all.c` reproduit
le hors-tableau sous AddressSanitizer ; `repeat_writes.py` compare deux
traces synthétiques. Aucune ROM utilisateur utilisée pour ces reproductions.

`cmake -G Ninja -B build . -DBUILD_TESTS=ON` et `ninja -C build` réussissent.
Les 20 tests ciblés codecs, cache, pile et profilage passent. Ils ne couvrent
pas les trois cas défectueux reproduits ci-dessus. `git diff --check` passe.
Les cinq contrôles complémentaires passent aussi : batch de compilation,
référence GG/SMS, e2e GG/SMS, e2e GB/META (dont ROM packed et balayage des
budgets de pile), cible Pokitto. Total : 25/25 tests sélectionnés.
La suite complète et les essais matériels n'ont pas été exécutés pendant
cet audit. Les tests hôte ne valident pas à eux seuls le code Thumb-1.
