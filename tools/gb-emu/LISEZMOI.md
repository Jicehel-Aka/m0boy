# gb-emu : émulateur Gamebuino META pour les firmwares gbrecomp

Fork de [gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
d'Andy O'Neill (licence MIT, voir `LICENSE`), à partir du commit `9631729`.
Deux ajouts, sans lesquels les firmwares gbrecomp ne tournent pas :

- **exécution du code depuis la SRAM** (décodage paresseux) : les firmwares
  y copient leurs boucles PPU en assembleur (`GBL_PPU_RAMFUNC`), et
  l'original plante à la première image rendue ;
- **carte SD SPI émulée** : une image FAT brute (`mkfs.fat`/`mcopy`) ou un
  simple dossier, dont une image FAT16 est construite à la volée ; les
  `.SAV` écrits par les jeux sont réécrits dans les fichiers du dossier
  (la progression est gardée). Les jeux streamés y lisent leur `.GB`.

Pas de son (TC4 et le DAC ne sont pas émulés). Le temps suit le modèle de
l'original (20 MIPS), pas la vitesse réelle de la console.

## Dans le navigateur

Sans rien installer : ouvre `dist/index.html`, puis

- « Choisir un dossier de jeu » : un dossier de `roms/meta/` (fait par
  `compile.sh`) devient la carte SD et son `.bin` est chargé ;
- ou dépose simplement un `.bin` (sans carte, seuls les jeux non streamés
  tournent).

Touches : flèches, J = A, K = B, U = MENU, I = HOME.

Avec le serveur de développement (Node) : `npm install`, puis
`SD_DIR=../../roms/meta npm start` et http://localhost:8080 (par défaut, la
carte est `roms/meta`).

## En ligne de commande (sans fenêtre)

```bash
npm install            # une fois (pour reconstruire ; dist-cjs/ est fourni)
node headless.js <firmware.bin> [ticks par lot] [lots] [carte_sd]
node headless.js "../../roms/meta/Tetris/Tetris.bin" 4000000 8 "../../roms/meta/Tetris"
```

`<carte_sd>` est une image `.img` ou un dossier. Le harnais affiche les
couleurs dominantes de l'écran après chaque lot : de quoi vérifier qu'un
firmware démarre et dessine.

## Reconstruire

```bash
npm install
npm run build        # dist/meta-emulator.js (page web)
npm run build:cjs    # dist-cjs/ (headless.js)
```
