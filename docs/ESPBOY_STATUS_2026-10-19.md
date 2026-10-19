# ESPboy — 19 octobre 2026

Cible ESP8266 (`--target espboy`, version indépendante 0.3.0). Le support
porte sur les cartouches Game Boy DMG, SG-1000, Game Gear et Master System
(section « Sega » plus bas). **Aucune validation sur console réelle de cette
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

## Sega (SG-1000, Game Gear, Master System)

Même démarrage, mêmes pilotes (`espboy_hw.cpp`) et même carte flash. Les
façades `sg_main.cpp` / `gg_main.cpp` gardent leur chemin META (lignes
envoyées par `meta_lcd_send_rows`, 160 → 128 colonnes dans le pilote) ; la
ROM est lue par des chargements alignés (`m0_load_u8`, z80.h).

- ROM ≤ 256 Kio : dans le firmware (`sg_rom` en PROGMEM). Au-delà (GG/SMS) :
  page 0 dans le firmware, pages 1 et suivantes brutes dans `rom.bin` à
  `0x100000`, secteurs de 512 octets lus par `gg_platform_rom_read` à travers
  le cache de cartes (6 secteurs en RAM). Pas de compression (refusé au-delà de
  2,75 Mio).
- RAM cartouche : 8 Kio en RAM (`GG_CART_RAM_8K` : miroir sur la fenêtre de
  16 Kio, donc les jeux à RAM de 16 ou 32 Kio ne sont pas corrects), écrite en
  flash 2 s après la dernière modification (somme de contrôle toutes les 8
  images), copies alternées comme pour le Game Boy. Ni états ni menu.
- Vérifié : `tests/test_sega_espboy.py` (projet précis natif = interpréteur
  pour GG, SMS et SG-1000 ; GG de 320 Kio avec `rom.bin` ; ROM refusée
  trop grande ; `espboy_sega_flash_check.cpp` sur flash simulée : secteurs de
  `rom.bin`, sauvegarde, copies abîmées) et builds Xtensa (`ESPBOY_PIO`).
- Lot converti (`TARGET=espboy ./compile.sh`, parties automatiques sur l'hôte) :
  Bank Panic, Bomb Jack (SG-1000) ; Pengo, Bubble Bobble, Asterix and the Great
  Rescue (GG) ; Action Fighter, Aladdin (SMS). RAM statique de 62 à 73 Kio sur
  81,9 Kio (tas libre de 9 à 20 Kio), firmware de 422 à 888 Ko sur 1 Mio,
  38 à 42 octets de flash par instruction compilée (estimations de build).

## Essais dans gm0

Le firmware ESPboy (le vrai binaire Xtensa, `firmware.bin` + `firmware.sym` +
`rom.bin`) tourne dans [gm0](https://github.com/r043v/gm0), qui exécute le code
du firmware et émule par fonction la ROM de la puce et le SDK Espressif
(`firmware.sym`, produit par le build, donne les noms). Ce n'est pas la puce :
le temps y est une estimation, le démarrage du SDK n'est pas exécuté, et
l'écran est un modèle ST7735 (panneau BGR : `MADCTL.BGR` à 0 échange rouge et bleu, le firmware pose 0xC8).

- Les 9 jeux convertis (Super Mario Land et 2, Bubble Bobble, Pengo, Asterix,
  Action Fighter, Aladdin, Bank Panic, Bomb Jack) démarrent depuis leur zip,
  affichent la bonne image avec les bonnes couleurs et ne s'arrêtent pas sur
  900 images ; les ROM de plus de 256 Kio sont lues de `rom.bin` par
  `spi_flash_read` ; les sauvegardes (flash `0x3C0000`) passent par l'émulation.
- Super Mario Land tourne à 59,0 images/s d'après `gbl_frame` et l'horloge
  estimée (le frontal attend l'échéance de l'image : cela dit que la machine
  estimée n'est pas en retard, pas combien de marge il lui reste).
- Un défaut a été trouvé ainsi : le firmware d'une cartouche sans RAM de
  sauvegarde ne compilait plus après l'ajout de la RAM Sega (`GBL_BATTERY`
  absent) ; `tests/test_espboy_target.py` a maintenant ce cas.
- `ESPBOY_GM0=/chemin/gm0` fait passer chaque firmware construit par gm0 dans
  les deux tests ESPboy.

## Comparaison avec l'émulateur ESPboy de référence

L'émulateur [ESPboy-edu/espboy_emulator](https://github.com/ESPboy-edu/espboy_emulator)
(MIT) exécute la vraie ROM de la puce (à dumper soi-même : elle n'est pas dans le
dépôt, donc je n'ai pas pu y lancer nos firmwares) et est recoupé avec une console
(instructions, durées, panneau). J'ai comparé le matériel que nos firmwares
supposent et le modèle de gm0 avec le sien :

- **Confirmé** : écran ST7735 128×128 sur HSPI (MOSI 13, SCK 14), D/C sur GPIO16,
  CS sur la broche B0 du MCP23017 (0x20), I²C sur GPIO4/5, MCP4725 (0x60) pour le
  rétroéclairage, haut-parleur sur GPIO0 par sigma-delta, boutons sur le port A
  actifs bas avec tirages, fenêtre visible = colonne 2, ligne 3 de la mémoire avec
  `MADCTL` 0xC8 (le même bloc que TFT_eSPI), panneau BGR (`MADCTL.BGR` à 1 =
  couleurs correctes). Le firmware m0boy n'a pas eu à changer.
- **Corrigé dans gm0** : durées d'instructions mesurées (MULL 2, branche +2, RET
  +3, chargement +5 hors DRAM, registre de périphérique +15 en lecture, +10 en
  écriture, interverrouillage chargement-utilisation, MEMW) ; le modèle de défaut
  de cache d'instructions que j'avais ajouté n'est pas dans la référence : il est
  éteint par défaut ; écran : adresses de colonne/ligne sur l'octet bas, SWRESET
  qui garde `MADCTL` et `COLMOD` (je le remettais à zéro), `COLMOD` 12/16/18
  bits (18 au reset), panneau blanc éteint, BGR appliqué au balayage, CS du
  MCP23017 ; interruption de FRC1 conditionnée au bit de DPORT ; lecture du port
  A sans tirage = 0.
- **Voulu, pas un écart** : les boutons. Les bits 4 (ACT) et 5 (ESC) donnent B et
  A du Game Boy, croisés exprès comme dans l'ancien émulateur : plus commode à
  jouer. La référence, elle, nomme le bit 4 « A ».
- **Non couvert ici** : le cache d'instructions (la référence ne le mesure pas),
  la durée réelle des écritures et effacements de flash, le LED WS2812 sur GPIO2.

## Fiche technique du ST7735S (V1.1, Sitronix, 2011-11-21)

Relue pour le modèle d'écran de gm0 (sections 9.10, 9.11.2, 10.1.2, 10.1.20,
10.1.21, 10.1.29, 10.1.33) et recoupée avec le code de TFT_eSPI utilisé par
l'ancien émulateur :

- CASET / RASET : la fiche dit que seuls XS[7:0], XE[7:0], YS[7:0], YE[7:0] sont
  lus au RAMWR : l'octet bas seul est donc dans la fiche, pas seulement une
  mesure sur le panneau.
- MADCTL : MY, MX, MV conformes à la table 9.11.2 (mémoire 132×132 : colonne ou
  ligne ↦ 131 − compteur) ; le bit 3 est « 0 = panneau à filtres RVB, 1 = panneau
  à filtres BVR » : l'ESPboy est un panneau BVR (TFT_BGR, MADCTL 0xC8).
- SWRESET : MADCTL et COLMOD ne changent pas (fiche : « S/W Reset – No Change ») ;
  COLMOD est à 18 bits à la mise sous tension ; l'écran est éteint (blanc) jusqu'à
  DISPON.
- TFT_eSPI, ST7735_GREENTAB3, rotation 0 : MADCTL = MX | MY | BGR, colonne de
  départ 2, ligne de départ 3 : exactement ce qu'envoie notre pilote.

## À vérifier sur la console

Écran (couleurs, décalage 2/3, orientation), boutons, son, saves et états,
quantité de tas libre avec le SDK, pile de 6 Kio (`CONT_STACKSIZE`), vitesse.
