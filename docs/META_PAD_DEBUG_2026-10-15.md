# META : débogage du pad et du bus SPI sur console réelle (2026-10-15)

Première session de tests du firmware bare-metal sur une Gamebuino META
réelle. État de départ : écran OK, **pad mort** sur tous les jeux, **écran
bleu** (carte illisible, `gbl_stream_init` err 1) sur les jeux streamés.
Écran, émulation et DMA fonctionnaient : seul le chemin *lecture* du SPI
(MISO, PA12 partagé par la carte SD et le pad) était en cause, l'écran étant
en écriture seule.

## Corrections logicielles validées (dans le dépôt, non commises)

| # | Fichier | Correction | Preuve |
| --- | --- | --- | --- |
| 1 | `targets/meta/platform/meta/meta_spi.c` (`meta_spi_init`) | PA12 (MISO) : `DIRCLR` + `PINCFG = PMUXEN\|INEN` ; les trois CS (SD PA27, LCD PB22, pad PB03) en sortie haute avant la première horloge | Sans INEN, MISO lit 0 : écran bleu + pad mort (les symptômes initiaux) |
| 2 | `targets/meta/platform/meta/meta_spi.c` (`meta_spi_rate`) | Suppression du **SWRST** du SERCOM à chaque changement de débit (séquence disable/reconfigure/enable comme `SPI.beginTransaction`) | Avec SWRST, le byte du pad était figé (barre blanche constante) ; plus de SWRST = le byte vit. Le SWRST libère les pads et le 74HC165 s'affole pendant le chargement parallèle |
| 3 | `targets/meta/platform/meta/meta_spi.c` (`meta_pad_read`) | Attendre **TXC** (shifter vide) puis drainer la FIFO RX, et **réanimer le récepteur** (reconfiguration complète du SERCOM) avant chaque lecture | En jeu, un frame d'octets d'affichage déborde la FIFO RX (2 niveaux) ; le SAMD21 lit alors des octets périmés jusqu'à ce que CTRLB/RXEN soit réécrit (impossible SERCOM actif). Sans ça : byte constant toute la partie |
| 4 | `targets/meta/platform/meta/meta_lcd.cpp` (`lcdq_start`) | La voie IRQ (bandes 2..N) réimpose `meta_spi_rate(TFT_SPI_HZ)` avant `set_window` | À 12 MHz pad, les bandes enchaînées par l'IRQ partaient au diviseur du pad (2× lentes) : gel + glitches d'affichage. À 24 MHz le bug était invisible (pad et écran au même diviseur) |
| 5 | `targets/meta/runtime/gbl_board.h` | Pad à **12 MHz** + `META_BUTTONS_PL_US 1` + **`META_BUTTONS_LAPINOU_ORDER 1`** ; `targets/meta/VERSION` → 0.10.12 | Voir « mapping » ci-dessous |

`targets/meta/platform/meta/meta_spi.c` contient en plus une impulsion de
resynchronisation CS (bas→haut→bas, bus calme) avant chaque lecture — testée
sans effet mesurable, gardée par prudence, à réévaluer.

## Mapping du pad sur la console testée

Mesures en jeu (menu debug `PAD:XX`, byte brut de `meta_pad_read`) :

    Gauche = bit0 (0x01)   Droite = bit1 (0x02)   Haut   = bit2 (0x04)
    A      = bit3 (0x08)   B      = bit4 (0x10)   Select/HOME et
    Start/MENU : bits 5/6 selon les builds (rotation ±1, voir ouvertes)
    Bas    : **jamais lu**, sur aucun build, aucun débit

C'est l'**ordre meta-lapinou** (0 left, 1 right, 2 up, 3 A, 4 B, 5 MENU,
6 down, 7 HOME) : `META_BUTTONS_LAPINOU_ORDER=1` (remappage déjà présent
dans `meta_main.cpp`) rend directions et A/B corrects. Sur cette console
les bits hauts ne collent pas à la table (Start/Select permutés, Bas absent)
— il reste une rotation ±1 sur les trois derniers bits, voir « ouvert ».

## Hypothèses testées et rejetées (avec les données)

- **Débit** : 24 MHz et 12 MHz donnent tous deux une rotation ; le choix de
  12 MHz est celui de la lib (marges doubles), pas un fix du mapping.
- **Settle du chargement parallèle** : 0, 1 µs (lib), 100 µs — aucun effet.
- **Double lecture** (une manche jetable puis la bonne) : le byte revient
  décalé d'un bit (Bas perdu) — le 165 n'est pas un anneau, la manche
  supplémentaire décale le flux.
- **Impulsion CS double bord** (charge garantie sur l'un ou l'autre bord,
  bus calme) : aucun effet.
- **DMA audio** (hypothèse utilisateur) : le canal 1 (TC4→DAC) n'a pas
  d'interruption activée et ne touche pas le SPI ; le padtest v8 tournait
  avec `m0_audio_init` (DMA jamais démarré, `gbl_apu_set_rate` stubbé) —
  différence réelle avec le jeu, mais non mécaniquement reliée au SPI.
- **La charge (PL) du 74HC165 n'est jamais observée en contexte jeu** : la
  lecture A (driver) et C (pulse PL de 100 µs) donnent le même byte, alors
  qu'un chargement réussi rendrait C correct. Toutes les manœuvres autour de
  CS étant restées sans effet, la charge effective — ou son absence — reste
  inexpliquée (voir ouvertes).

## Ce qui marche comme référence

Le **padtest** (`out/hw-padtest`, firmware minimal : écran + boucle de
lecture) lit les 8 touches proprement dans une boucle serrée, avec la même
séquence de boot (SD + horloges + audio init). C'est le contexte bus (une
seule bande par boucle, lectures rapprochées) qui diffère du jeu (32 bandes
TBLEND + commandes de fenêtre + émulation). Le padtest v7 embarquait un CDC
USB minimal (énumération OK une fois, touch 1200 bauds → sam-ba par
effacement de la première ligne d'app) — jamais fiable, à reprendre si besoin.

## État du dépôt

- Modifiés : `meta_spi.c`, `meta_hw.h` (propre), `meta_lcd.cpp`,
  `gbl_board.h`, `meta_main.cpp`/`meta_menu.cpp` (versions projet de debug
  dans `out/hw-pad-sml`, pas dans le dépôt), `VERSION` (0.10.12), `README.md`.
- `ctest` : 29/29 (2 skipped sans rapport).
- Builds flashés : `out/hw-pad-sml` (SML plain), `out/hw-sd-sml` (SML streamé
  forcé → SUPERMAR.GB ; **jamais testé sur console**, prévu : bleu = carte
  morte, rouge = carte OK fichier absent).
- Dump de la console avant modifications : `out/meta-hardware/*.bin`.

## Procédure de flash (acquise péniblement)

```sh
BOSSAC=~/.platformio/packages/tool-bossac/bossac
# console en sam-ba : double-appui RESET ; single appui = relance l'app
$BOSSAC -p /dev/ttyACM0 -o 0x4000 -e -w -v <projet>/.pio/build/meta/firmware.bin -R
```

- Après un flash + `-R`, le bootloader peut rester en sam-ba (logique
  double-tap, magic 0x07738135 en RAM à 0x20007FFC) : un **appui simple**
  relance l'app ; la v7 du padtest l'efface au démarrage.
- La META tournant sur piles, **débrancher l'USB ne l'éteint pas** :
  interrupteur latéral OFF/ON = seul reset déterministe (POR).
- Le CDC sam-ba se bloque si un processus tient le port (`fuser
  /dev/ttyACM0`) ou si on l'interroge trop vite : tuer les lecteurs, un seul
  essai bossac, sinon redouble-appui. sam-ba v2.0 [Arduino:XYZ].
- L'app bare-metal n'a pas d'USB : chaque reflash demande un double-appui.
- Le build de la région loader (0x2000-0x4000) de cette console n'est pas le
  loader Gamebuino stock (aucune chaîne ASCII dans le dump) : le firmware
  n'hérite d'aucun état des pins du loader, d'où le correctif INEN.

## Ouvert (dans l'ordre)

1. **Finir le mapping des 3 touches hautes** (Select/Start/Bas) : le menu
   debug `PAD:XX` est dans `out/hw-pad-sml/src/meta_menu.cpp` (et la ligne
   `option_menu()` au boot dans `meta_main.cpp`, à retirer) ; relever le byte
   au repos + Start + Select + Bas à 12 MHz avec le fix LCD-rate, puis
   ajuster la table lapinou de `meta_buttons` (`meta_main.cpp`).
2. **Bas jamais lu** : présent dans aucune fenêtre, aucun débit. Piste :
   chaîne à plus de 8 étages (2× 74HC165 cascadés, le 2e octet = [L R U A B
   M H ?], Bas au-delà) — tester une lecture de 24 bits et localiser chaque
   bouton avant de choisir la fenêtre. Vérifier aussi le bouton Bas lui-même
   (son déclin au fil de la session — fantôme puis perte — ressemble à un
   faux contact).
3. **La charge PL du 165 en contexte jeu** : pourquoi CS (quelle que soit la
   manœuvre) ne resynchronise pas le registre en jeu alors que le padtest
   lit proprement. Récupérer le schéma META (câblage PL/CS du 165).
4. **USB CDC du padtest** : l'énumération n'a marché qu'une fois (PID
   2341:804d, PMUX fonction G 0x66, descripteur config 75 octets,
   multi-paquet IN) — à déboguer si on veut le flash sans double-appui.
5. **SD** : jamais testée sur console (l'écran bleu initial = MISO mort).
   Reflasher `out/hw-sd-sml` (rebuild avec tous les correctifs) : bleu =
   carte illisible, rouge = `SUPERMAR.GB` absent, boot = OK.

## Suite du 2026-10-16 : pad, crash, carte SD, USB (version 0.10.13)

Méthode : un firmware de diagnostic (hors dépôt) garde un journal en RAM
`.noinit` (lectures du pad, réponses SD, cadre de pile d'une faute,
RCAUSE, WDT) ; après un double appui sur RESET, le bootloader laisse la RAM
intacte et la commande sam-ba `R<adresse>,<nombre>#` la relit. Puis, une
fois l'USB du firmware en place, la même commande pendant le jeu.

| Symptôme | Cause mesurée | Correction |
| --- | --- | --- |
| Touches décalées, Bas jamais lu | La lecture de 0.10.12 (redémarrage du récepteur + impulsion CS avant la lecture) rend l'octet décalé d'un bit : Bas sort du registre. La lecture de la bibliothèque (12 MHz, CS bas, 1 µs, un octet) donne les 8 touches dans l'ordre de la bibliothèque (0 bas, 1 gauche, 2 droite, 3 haut, 4 A, 5 B, 6 MENU, 7 HOME), en jeu comme sur l'écran d'erreur, et le second octet d'une lecture 16 bits vaut 0x00 (entrée série du 165 à la masse) | `meta_pad_read` revenu à la lecture de la bibliothèque ; ordre meta-lapinou désactivé par défaut |
| Crash « au bout d'un court moment » (tous les jeux) | HardFault 400 ms après le démarrage, PC dans `m0_audio_frame` → `channel_start`, sur l'écriture `DMAC->CHCTRLA = SWRST` du canal audio : la chaîne de descripteurs arrivée au bout laisse le canal activé (suspendu), et ce SWRST sur un canal activé fait une faute de bus sur le SAMD21 réel. PAC (protection d'écriture) mesuré non armé, DMAC trouvé éteint au démarrage : pas en cause | Le canal est désactivé (attente de ENABLE = 0) avant le SWRST. Mario 197 s, 11 779 images, aucune faute |
| Écran bleu des jeux streamés | Carte identifiée (CMD0/8/41 à 250 kHz), mais chaque CMD17 à 24 MHz échouait (`M0_FAT_IO`) : le SPI maître du SAMD21 ne lit pas MISO de façon fiable au-delà de ~12 MHz (le noyau Arduino plafonnait tout à 12) | `META_SD_SPI_HZ` = 12 MHz : la FAT se monte et les dossiers se parcourent (vérifié jusqu'au « fichier absent » ; un jeu streamé de bout en bout n'a pas encore tourné, voir ouvert) |

Autres corrections, sans symptôme isolé : `meta_spi_rate` attend que le
dernier octet ait quitté le registre (TXC) avant de couper le SERCOM ;
`meta_pad_read` attend lui-même la fin des bandes de l'écran (au démarrage,
la palette lisait le pad pendant les bandes, CS écran bas) ;
`DMAC_Handler` restaure CHID.

Hypothèses de 0.10.12 démenties par les mesures : la « rotation du byte
par le trafic écran » (la lecture simple ne tourne pas, en jeu), la
nécessité de réanimer le récepteur (inutile), l'ordre meta-lapinou (c'est
l'artefact du décalage d'un bit), le bouton Bas défectueux (il marche).

### USB (`META_USB`, `meta_usb.c`)

CDC-ACM nu (VID:PID 2341:804D, « Gamebuino META m0boy »). Le bootloader
de cette META (désassemblé depuis la flash, 0x1964) : après un reset
logiciel il lance l'application quoi qu'il y ait en RAM, sauf si
0x20007FFC vaut **0x3aecfd88** (la demande « enter bootloader » du
launcher) ; après un reset watchdog il lance le **launcher** ; le magique
double-tap Arduino (0x07738135) ne compte qu'après un appui sur RESET. Le
firmware écrit donc 0x3aecfd88 puis `NVIC_SystemReset`. Vérifié : deux
cycles complets jeu → touche 1200 bauds → sam-ba → flash → jeu, sans
toucher la console. Le firmware reste détaché 200 ms au démarrage, sinon
l'hôte (Linux derrière un hub) gardait le périphérique du bootloader.

Matériel : le hub VIA VL813 a perdu la trace de la META (périphérique
fantôme bloquant le `hub_wq` jusqu'à son débranchement) ; ensuite, liaison
intermittente (erreurs -71, détection « low-speed ») avec le câble ou le
connecteur de la META, bootloader compris.

### Jeux streamés : le mauvais `ROM.*`

Une fois la carte lisible, Ristar affichait encore une erreur : le code 4
« autre ROM » (bleu dans la table RGB565 du frontend GG, vu **rouge** sur
cette dalle câblée BGR ; de même l'erreur 1 « bleue » du frontend GB était
vue rouge). Chaque dossier de jeu de la carte a son `ROM.GG` (convention de
compile.sh) et la recherche prenait le premier trouvé (celui d'Aladdin,
256 Kio, relu en direct par USB : une seule extension de 512 secteurs).
`m0_fat_find_in` (meta_fat.c) appelle désormais un filtre sur chaque
candidat : le frontend GG garde le fichier dont le premier secteur est celui
de la ROM en flash, le frontend GB celui dont l'en-tête de cartouche
correspond ; la sauvegarde GG (`ROM.SAV`) est cherchée dans le dossier de la
ROM retenue. Test : `tests/test_meta_fat.py` (trois dossiers de jeux).
Ristar 48 (sources 0.10.13, sans profil de couverture donc presque tout
interprété, d'où la lenteur) démarre et se joue depuis la carte.

### Ouvert

1. Ristar construit par compile.sh (avec ses profils) pour juger la vitesse
   réelle avec la carte à 12 MHz.
2. Les sauvegardes GB (`<NOM>.SAV`, `.STA`) et les GG sans ROM sur la carte
   prennent encore le premier fichier du nom : les variantes 48 et 72 d'un
   même jeu partagent ce nom. Piste : reconnaître le dossier du jeu par son
   `.BIN` (premier secteur identique au firmware en flash à 0x4000).
3. Le hub VIA VL813 ne voit pas le redémarrage de la META après un flash
   (`bossac -R`) et se fige ; en direct sur le PC, aucun souci.
4. Le retour au bootloader depuis un écran d'erreur (avant l'audio) n'a
   pas été revu sur console après la correction de `usb_poll`.

## Suite du 2026-10-17 : noms 8.3, USB de débogage, fautes, 72 MHz

- **Fichiers du jeu** : tous nommés comme le `.BIN`, d'un nom court 8.3
  (`--sd-name`, choisi par compile.sh et compile.mjs : lettres et chiffres
  du nom du jeu, au-delà de 8 les 6 premiers et 2 chiffres du SHA-1 de la
  ROM). Le dossier garde son nom long (le loader l'affiche et prend le
  premier `.bin` du dossier). Le firmware retrouve son dossier par son
  `.BIN` (premier secteur identique à la flash en 0x4000,
  `m0_fat_game_dir`) et y cherche ROM, sauvegarde et états : les
  variantes 48/72 d'un même jeu peuvent porter le même nom. Firmware flashé
  par USB, différent de celui de la carte : repli sur la recherche par
  contenu de la ROM. Cartes existantes : renommer `ROM.SAV` / l'ancien
  `.SAV` du jeu au nouveau nom pour garder la progression.
- **USB de débogage** (`META_USB`, `tools/meta-usb.py`) : lecture/écriture
  mémoire par symbole, profilage (anneau de 32 PC échantillonnés par
  SysTick : ~1000 échantillons/s, 0 perdu, 0,6 % du CPU mesuré), touches
  injectées (le menu a été piloté à distance), journal des fautes, flash
  sans toucher la console.
- **Fautes et blocages** : journal `.noinit`, redémarrage automatique
  (mesuré : HardFault -> jeu relancé ; boucle principale bloquée par une
  écriture USB de `pace_owed` -> alerte du watchdog à 2 s, PC dans
  `m0_micros`, jeu relancé ~4 s après). Piège trouvé : écrire
  `WDT->CLEAR` pendant une synchronisation bloque le bus plusieurs ms
  (`m0_watchdog_feed` teste SYNCBUSY) ; le générateur 2 n'a qu'un
  diviseur 5 bits (DIVSEL).
- **72 MHz** : inutilisable sur cette console (suite plus bas : 70 MHz). HardFault dans
  `gbl_ppu_fetch_asm` au premier rendu, sur un pointeur impair que le code
  ne peut pas produire (`trow` toujours pair) ; essayé : PLL depuis la
  DFLL48M, PLL depuis le quartz 32 kHz avec LBYPASS, RWS 2 et 3, routine en
  flash au lieu de la SRAM (blocage total). À reprendre avec les réglages
  de l'émulateur GB de l'utilisateur qui tenait 72 MHz.

### 70 MHz (version 0.10.14)

Le vieil émulateur GB de l'utilisateur (drmario.bin, désassemblé) : RWS 2,
FDPLL96M à **70 MHz** (LDR 69) depuis la DFLL48M / 48 par un GCLK, sans
LBYPASS ; USB déplacé sur un générateur DFLL ; boutons lus à 8 MHz parce que
son SPI suivait le PLL. Même méthode que m0boy, sauf 70 au lieu de 72.
Sur console : 70 MHz avec les boucles PPU en SRAM -> HardFault en moins
d'une minute (dans `gbl_ppu_convert_fine_asm`, `gbl_ppu_convert4_asm`,
en SRAM) ; 70 MHz avec ces boucles en flash -> plus de 3 minutes sans
faute (deux essais), CPU 70 % en attente d'image sur Super Mario Land.
Retenu : `OC_MHZ` 70, `GBL_PPU_RAMFUNC` 0 sur la META (en SRAM : 3,9 % du
CPU à 48 MHz, en flash : 4,8 %, profileur USB). Les boutons gardent
~12 MHz : le SERCOM n'est sur le PLL qu'avec l'écran au-delà de 24 MHz
(70/6 = 11,7 MHz).

### Défauts, jeux streamés de bout en bout (version 0.10.15)

- **Par défaut** : processeur à 70 MHz (`META_OVERCLOCK` 1 sur la META ;
  compile.sh `OVERCLOCK=0` pour 48), écran au maximum : `META_LCD_SPI_MHZ`
  48, soit 35 MHz avec le processeur à 70 (une seule PLL, SPI = horloge du
  SERCOM / 2 au plus : impossible de dépasser 35 à 70 MHz). Mesuré sur
  Ristar (écran titre) : 70 + 35 laisse 12,7 % du processeur en attente
  d'image, 48 + 48 : 6,1 %.
- **Couleurs** : rouge et bleu permutés sur cette console (écrans d'erreur
  GB et GG, jeux GG) ; `META_LCD_BGR` 0 par défaut (bit BGR de MADCTL
  retiré), couleurs confirmées sur Ristar.
- **Rendus SG-1000 / Game Gear** : boucles en flash sur la META
  (`SG_CODE_IN_SRAM` 0, écrit par le générateur), comme le PPU GB.
- **Game Gear streamé** : HardFault au démarrage de Ristar construit par
  compile.sh : la détection Codemasters lisait `rom[0x7fe6]` alors que la
  flash ne garde que la page 0 (16 Kio, placée en fin de flash pleine :
  lecture au-delà de 0x40000). Les projets générés fixent `GG_MAPPER` : la
  lecture n'a plus lieu. Ristar et Sonic tournent depuis la carte à 70 MHz
  (dossier trouvé, ROM trouvée, aucune faute, 2 min de jeu injecté).
- **Super Mario Land 2** (GB, ROM et RAM de cartouche sur la carte) : la
  RAM de 8 Kio sert de mémoire de travail (environ sept secteurs) ; avec
  un cache de 2 secteurs : 157 écritures/s et la moitié du processeur.
  Cache de RAM paramétrable (`GBL_ERAM_CACHE_SECTORS`, LRU, le secteur
  précédent vérifié avant le balayage) ; 2 secteurs de ROM + 8 de RAM
  dans ces builds (même mémoire) : écritures ramenées au vidage de 2 s,
  39 à 42 images/s (24 avec 4 + 6). Le chemin d'accès à cette RAM reste
  coûteux (`eram_sector`, `gbl_rd_slow` : environ 30 % du processeur).
