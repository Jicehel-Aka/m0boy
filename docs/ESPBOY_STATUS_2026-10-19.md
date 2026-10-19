# ESPboy — 19 octobre 2026

Cible ESP8266 (`--target espboy`, version indépendante 0.2.0). Le support
porte sur les cartouches Game Boy DMG ; les systèmes Sega ne sont pas encore
portés sur cette destination. **Aucune validation sur console réelle de cette
version** : la 0.1.0 (première version, démarrage sans SDK) a été remplacée
par un démarrage Arduino standard, calqué sur l'ancien émulateur ESPboy qui,
lui, fonctionnait sur la console.

## Implantation

- Flash : 4 Mio (ESP-12F). Programme (cœur Arduino, runtime, code généré) au
  plus 1 Mio à `0x0` : c'est tout ce que le CPU mappe. Une ROM jusqu'à 256 Kio
  en fait partie. Au-delà, la banque 0 y reste et le reste (secteurs 32 et
  suivants) est dans `rom.bin` à `0x100000`, 2 816 Kio, lu seulement avec
  `spi_flash_read` à travers le cache de secteurs du runtime : brut tant que
  tout tient, sinon seuls les secteurs de 512 octets nécessaires sont
  compressés (LZSS, plus gros gain d'abord). Une ROM trop peu compressible est
  refusée. Sauvegardes à `0x3C0000`, 236 Kio. Secteurs SDK à `0x3FB000`.
  Constantes dans `targets/meta/runtime/gbl_espboy.h` (copie dans
  `meta_emitter.cpp`, comparée par le test).
- RAM cartouche sur pile (16 Kio au plus) : deux copies alternées en flash,
  en-tête (séquence, somme de contrôle) écrit en dernier, écriture 2 s après
  la dernière modification, à la désactivation de la RAM par le jeu et en
  quittant ; jamais périodique (usure). États : autant d'emplacements que la
  zone en contient (7 pour une RAM de 8 Kio), effacés bloc par bloc, en-tête
  en dernier, somme de contrôle vérifiée avant chargement. Identité de jeu :
  FNV-1a de la ROM.
- Matériel repris de l'ancien émulateur : ST7735 (DC GPIO16, CS par GPB0 du
  MCP23017, SPI 40 MHz), boutons du MCP23017 (bit 5 = A, bit 4 = B, 6 =
  select, 7 = start), son par générateur sigma-delta sur GPIO0 alimenté à
  11 025 Hz par timer1 (porteuse 312,5 kHz au lieu de ~11 kHz), watchdogs
  désactivés, exécution dans `setup()`.

## Éléments vérifiés

Tous les essais utilisent des cartouches synthétiques créées par les tests,
sans ROM commerciale ni fichier de firmware ajouté au dépôt.

- `tests/test_espboy_target.py` : génération ESPboy → META dans un même
  dossier, égalité du `run_hash` avec l'interpréteur en mode précis, cohérence
  des constantes de la carte flash (en-tête, convertisseur, linker), ROM de
  512 Kio stockée brute en mode lecture de secteurs (banque 0 seule dans le programme), lecture de chaque octet
  d'une cartouche MBC5 de 4 Mio à travers le cache (chemin `espboy_rom_read`
  simulé sur l'hôte), refus d'une ROM incompressible, de la SD et de
  `--meta-thumb`.
- Sur la cartouche de 4 Mio : 1 991 secteurs compressés, 6 169 bruts hors
  banque 0.
- `tests/espboy_flash_check.cpp` (flash simulée stricte : alignement,
  programmation uniquement sur octets effacés) : copies alternées, coupure de
  courant à chaque étape, copie abîmée, 7 emplacements d'état, coupure
  pendant une sauvegarde d'état, état abîmé refusé.
- Build Xtensa (GCC 10.3, PlatformIO espressif8266 4.2.1, `ESPBOY_PIO`) du
  projet synthétique (3 400 instructions natives) : 443 776 octets de binaire,
  72 852 octets de RAM statique sur 81 920 (environ 9 Kio de tas libre : à
  surveiller sur la console). Ce sont des tailles de build.

## À vérifier sur la console

Écran (couleurs, décalage 2/3, orientation), boutons, son, saves et états,
quantité de tas libre avec le SDK, pile de 6 Kio (`CONT_STACKSIZE`), vitesse.
