# ESPboy — 19 octobre 2026

Première cible ESP8266 (`--target espboy`, version indépendante 0.1.0).
Le support porte sur les cartouches Game Boy DMG ; les systèmes Sega ne sont
pas encore portés sur cette destination. Aucune validation sur console réelle.

## Implantation

- Flash physique : 1 Mio. Programme, eboot compris : au plus 256 Kio,
  contrôlés par le linker et par la taille du `.bin` produit.
- ROM séparée à `0x40000` : 748 Kio ; les 20 Kio à partir de `0xFB000`
  restent réservés. PlatformIO flashe les deux images dans la même commande.
- Brut prioritaire. Si la ROM dépasse cette zone, seuls les secteurs de
  512 octets nécessaires sont compressés, par gain décroissant de place.
  La banque 0 reste brute. Codec LZSS et cache du runtime existants ; aucune
  lecture SD de secours. Une ROM trop peu compressible est refusée.
- Runtime Game Boy, générateur C, menu, palettes et réduction verticale
  partagés. Le pilote LCD réduit aussi la largeur 160 → 128, sans découpe.
- Démarrage sur la pile du boot ROM, pilotes SPI/I²C et timer issus du cœur
  Arduino ESP8266 3.1.2, sans lancer le SDK réseau. Temps par CCOUNT à
  80/160 MHz ; timer1 pour l'audio sur GPIO0. Ce démarrage reste à tester
  matériellement, comme les couleurs, les boutons et le son.
- RAM cartouche en RAM ; sauvegardes à pile et états non persistants.

## Éléments vérifiés

Tous les essais utilisent des cartouches synthétiques créées par les tests,
sans ROM commerciale ni fichier de firmware ajouté au dépôt.

- `tests/test_espboy_target.py` : génération et passage ESPboy → META dans
  un même dossier, brut prioritaire même avec `--meta-pack`, égalité du
  `run_hash` avec l'interpréteur en mode précis, lecture de chaque octet
  d'une cartouche MBC5 de 1 Mio à travers le cache, refus de la SD et des
  variantes ARM, refus d'une ROM incompressible trop grande.
- Sur cette cartouche de 1 Mio : 614 secteurs compressés et 1 402 secteurs
  conservés bruts hors banque 0 ; image ROM de 765 524 octets. Le stockage
  est donc partiellement compressé, et pas compressé systématiquement.
- Build Xtensa réel, GCC 10.3 / PlatformIO espressif8266 4.2.1 : programme
  précis avec 3 400 instructions natives, cas compressé : **220 608 octets**
  de binaire et 48 900 octets de RAM statique rapportés. Ce sont des tailles
  de build, pas des mesures de vitesse ou une preuve de fonctionnement
  matériel.
- `TARGET=espboy AUTO=0 PLAY=0 EXPLORERS=0 ALL=1 NO_ICONS=1 ./compile.sh`
  sur le fixture : archive contenant `firmware.bin`, `rom.bin`, `FLASH.txt`
  et le README, sans fichiers SD ; compilation réussie.
- Non-régression : suite de 46 tests, 45 réussis, dont Pokitto (liens GCC et
  Clang) et l'e2e META avec compression et balayage de pile. Le test Game
  Gear a dépassé sa limite CTest de 300 s pendant l'exécution parallèle ;
  relancé directement, il réussit pour GG et SMS, les pools bruts/compressés
  et la RAM cartouche.
- Neutralité META : deux liens ARM avec les mêmes options et le même projet
  synthétique, en remplaçant les fichiers partagés par ceux de HEAD,
  produisent des `firmware.bin` identiques octet par octet. Pas de changement
  de version META/Pokitto.
