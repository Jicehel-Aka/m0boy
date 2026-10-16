# META : débogage du pad et du bus SPI sur console réelle (2026-10-15)

Première session de tests du firmware bare-metal sur une Gamebuino META
réelle. État de départ : écran OK, **pad mort** sur tous les jeux, **écran
bleu** (carte illisible, `meta_stream_init` err 1) sur les jeux streamés.
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
  avec `meta_audio_init` (DMA jamais démarré, `gbl_apu_set_rate` stubbé) —
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
