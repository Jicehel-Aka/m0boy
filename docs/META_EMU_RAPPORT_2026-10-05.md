# META : émulateur fidèle, runtime 0.5.0 — rapport et tâches (2026-10-05)

Rapport de la session du 2026-10-05 (après-midi) sur `tools/meta-emu-sdl`
et le runtime META (`targets/meta`), suivi de la liste de tâches pour
l'agent suivant (GLM).  Branche `meta-target`, **rien n'est commité** :
voir « État du dépôt ».

Règle donnée par l'utilisateur et appliquée partout : **le vrai matériel
d'abord** — l'émulateur ne reproduit plus les écarts de l'émulateur
TypeScript d'origine.  Les chiffres de cycles viennent du modèle de
l'émulateur (Cortex-M0+ 48 MHz + cache NVM) : ce sont des **estimations**,
jamais des mesures sur console.

---

## 1. Ce qui a été corrigé (symptômes signalés)

| Symptôme | Cause racine | Correctif |
|---|---|---|
| Écran GB : lignes de tiles en double, restes d'écrans | IRQ DMAC levée pour le canal audio (aucun CHINTENSET) : le `DMAC_Handler` du runtime la prenait pour une fin de bande écran | `dmac_raise` : IRQ seulement si CHINTENSET l'active |
| Son haché (jeux GB 0.5.0) | SWRST ne remettait pas le canal DMAC à zéro : un SUSP collé, `channel_running()` faux, 60 relances/s | `dmac_chan_swrst` + CHSTATUS.FERR + fin de chaîne DESCADDR=0 sans SUSP |
| Trous audio côté PC | file hôte plafonnée à 900 sous un callback SDL de 1024 | file calée sur le buffer obtenu + régulation dynamique ±0,5 % |
| Picomon : saturation + claquements sans fin après une musique | CMD18/CMD12 absents : `file.read()` échouait après le rembobinage, le canal WAV rejouait 2047 octets figés ; DAC x96 écrêtait | CMD18/CMD12 ; DAC x64 |
| Freeze en mode SMOOTH (~13 s) | IRQ injectées sous CPSID et dans le handler en cours : complétion LCD perdue, `claim_strip` bloqué | NVIC fidèle (voir §2) |
| « perte de fps » / lenteur | CPU émulé 1,5x trop lent (1 instr = 1 tick à 20 M) ; décodeur hérité lent | cycles M0+ à 48 MHz + cœur réécrit (1,5-1,9x plus rapide) |
| Noms longs FAT tronqués (`TITLESCREENB`) | caractères 12-13 mal placés, cluster LFN non nul | spéc VFAT |
| Zip Pokitto `.pop` (Galaxy Fighters) non chargé, puis sans musique | le zip ne cherchait que des `.bin` ; la carte n'avait pas la chaîne « FAT16 » à l'offset 54 (PetitFatFs : 7 lectures du secteur 0 puis échec) et, en superfloppy, une entrée MBR écrite dans le secteur de boot | `.pop` accepté dans les zips ; BPB étendu complet |

## 2. Émulateur : fidélité matérielle

- **Cœur ARMv6-M réécrit** (`step`, `switch (op >> 8)`), cycles intégrés
  (ALU 1, load/store 2, branchement pris 2, BL 3, PUSH/POP/LDM/STM 1+N,
  POP {pc} 3+N, MRS/MSR 4) + **cache NVM** (8 x 64 bits, RWS=1).  Écarts TS
  corrigés : MULS (double JS), décalages par registre et ROR, ADD/MOV/CMP
  sur registres hauts (sans flags, `add pc`, `mov pc`), REVSH, LDMIA sans
  écriture de base si Rn est chargé, BL complet, MRS/MSR réels.
- **NVIC** : PRIMASK (CPSID/CPSIE/MSR), priorités IPR/SHPR3, ISER/ICER/
  ISPR/ICPR, ICSR, pas de préemption à priorité égale, EXC_RETURN imbriqué,
  trame alignée 8 octets, xPSR réel, latences 15/10 cycles.
- **DMAC** : CHINTEN, SWRST, FERR, fin de chaîne.
- **SERCOM4** : octet = 16 x (BAUD+1) cycles (DMA et CPU), INTFLAG
  DRE/TXC/RXC temporisés, tampon RX 2 niveaux vidé par SWRST/ENABLE=0/
  RXEN=0.  Piège rencontré : sans le vidage au SWRST (Arduino `SPI.config()`
  à chaque changement de vitesse), les jeux lib lisaient des boutons
  fantômes et perdaient leur son.
- **Carte SD** : CMD18/CMD12.  **ST7735** : COLMOD 12/16/18 bpp.
- **Audio hôte** : file + régulation dynamique.  **FAT** : LFN, `used[][13]`
  (débordement possible au-delà de 256 fichiers corrigé).
- **Nettoyé** : modèle « site » (EMU_SITE), domaine TS (EMU_TS_TIMING,
  EMU_TICKS_HZ), MUL TS, STATE_HASH/ihash/TRACE_ALL, montres mémoire,
  `emu.c.txt`, `getenv` par octet SPI.
- **Outils** : `EMU_NOPACE`, `EMU_AUDIO_STATS`, `EMU_INPUT` (script
  d'appuis), `EMU_PROF` + `prof_report.py` (profil en cycles par fonction),
  `EMU_SPI_INSTANT`.
- **Vitesse** (Ryzen 7 6850H, timing fidèle 48 MHz) : natif 270-410 % du
  temps réel, wasm (V8) 160-220 % (le wasm appelle `step` par pointeur :
  V8 ne promeut pas sous TurboFan une boucle sans appels, pas d'OSR).

Validation : 15 firmwares (Picomon, Celeste, Yatzy, Cats & Coins, GTA
demake, Reuben Quest, lapinou, Tetris 0.4.x, Golf, Tetris, Mario Land,
Dr. Mario, Alleyway, Zelda, + Alleyway Pokitto), 0 PC fou, écrans et
statistiques audio vérifiés ; SMOOTH stable 4 000 frames.

## 3. Runtime META

- **`META_TBLEND`** (1) : TBLEND aussi en build streaming (672 o : lignes
  tenues + table de 16 couleurs) ; le défaut n'est plus TILE.  Mario Land
  streaming : 2 924 o de RAM libre pour la pile.
- **`META_LCD_12BPP`** (0) : ST7735 en RGB444.  `META_COLOR` donne
  0x0RGB, paquetage en Thumb-1 (`meta_lcd_pack_asm.S`, ~5 cycles/pixel).
  Estimation Mario Land : attente écran 8,5 % → 1,3 % du CPU, relances audio
  7 → 1 par minute, paquetage ~0,1 %.  Couleurs sur 4 bits par canal.
- **APU** : LFSR du bruit avancé par blocs de 14 (`gbl_apu_lfsr.h`), exact.
  Mario Land : APU 28,4 % → 12,7 % du CPU, relances audio 134 → 7 par minute.
- **SD** : `meta_sd_recv` en pipeline (DRE/RXC).  Effet non mesurable sur
  Mario Land/Zelda (lecture SD ≈ 0,2 %).
- **Startup** : synchro GCLK manquante (générateur 3) ajoutée.
- **Revue bare metal** : pas de framework (CMSIS seul), `millis()`/
  `delay()` réimplémentés dans `meta_startup.c` ; code sain.  Firmwares
  antérieurs au 2026-10-03 (ex. GARGOYLE.BIN du 02/10) = builds Arduino,
  SPI écran à 12 MHz : à reconvertir.
- **Tests** : `meta_display_stream_profile_matches_reference`,
  `meta_display_12bpp_matches_reference`, `meta_lcd_pack444_matches_st7735`,
  `meta_apu_lfsr_matches_per_clock` ; `ctest -R meta` : 12/12.

Profil de référence (Mario Land, 1 min, firmware actuel) : CPU libre 46,5 %,
APU 12,7 %, attente écran 8,5 %.  Zelda (streaming) : CPU libre 9 %,
interpréteur GB ≈ 45 % (`gbl_interp_block`, `H_LDH_LD`, `H_ALUN_AND`,
`H_JRNZ` : boucle d'attente STAT/LY interprétée), 820 relances audio/min.

## 4. État du dépôt (rien de commité)

Modifiés : `tools/meta-emu-sdl/{meta_emu.c,Makefile,README.md,
HANDOFF-2026-10-05-jeux-gb-050.md,NOTES-SD-LIB-OFFICIELLE.md}`,
`targets/meta/{README.md,package/CMakeLists.txt,platform/meta/*,
platform/src/*,runtime/{gbl_apu.c,gbl_profile.h},tests/display_sim.c}`,
`tests/CMakeLists.txt`.  Nouveaux : `prof_report.py`,
`targets/meta/platform/meta/{meta_lcd_pack.h,meta_lcd_pack_asm.S}`,
`targets/meta/runtime/gbl_apu_lfsr.h`, `targets/meta/tests/{lcd_pack.c,
apu_lfsr.c}`, ce rapport.  Supprimé : `tools/meta-emu-sdl/emu.c.txt`.
Identité de commit : `git -c user.name=r043v -c user.email=noferov@gmail.com`
+ trailer Co-Authored-By ; ne pas pousser ; ne pas toucher aux fichiers
non suivis de `tools/meta-emu-sdl` (zips, PDF, picomon/…).

`ctest` complet : 2 échecs hors META, préexistants à ce travail et non
liés à ces fichiers (`accuracy_runner_exit_policy` : catalogue de ROMs de
test absent ; `crystal_challenge_rules_are_bounded_and_deterministic` :
aucune sortie) — à confirmer (tâche T13).

---

## 5. Tâches pour GLM

Chaque tâche : objectif, commandes, critère d'acceptation.  Ordre = priorité.
Commandes depuis `/home/m/dev/gb-recompiled`, env de test
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

### P0 — à faire d'abord

**T1. Commit propre (après accord de l'utilisateur).**
Petits commits indépendants : (a) émulateur (cœur, NVIC, DMAC, SPI, SD,
audio, nettoyage, profileur), (b) runtime TBLEND streaming, (c) runtime
12 bpp, (d) APU LFSR, (e) SD pipeline + startup, (f) docs.  Avant chaque
commit runtime : `ninja -C build && ctest --test-dir build -R meta`.
Acceptation : `git log` lisible, `ctest -R meta` vert à chaque commit.

**T2. Régénérer tout le lot et vérifier la RAM.**
`NO_ICONS=1 ./compile.sh` (tout `roms/gb`) ; relever `RAM:` de chaque
`out/<jeu>` (`arm-none-eabi-size .pio/build/meta/firmware.elf`).  TBLEND
ajoute 672 o aux builds streaming : aucun jeu ne doit descendre sous ~2 Ko
de pile (`32768 - data - bss`).  Lister les jeux sous 2,2 Ko.
Acceptation : tableau RAM libre par jeu ; décision documentée pour les
jeux trop justes (`-DMETA_TBLEND=0` dans leur projet, ou gain de RAM).

**T3. Reconvertir les firmwares d'avant le 2026-10-03.**
Repérer `roms/meta/*/.gbrmeta.json` dont `version` < 0.5.0 ou date
< 2026-10-03 (ex. Gargoyle's Quest) ; reconvertir.  Acceptation : plus de
build Arduino dans `roms/meta`, Gargoyle sans famine audio dans l'émulateur
(`EMU_AUDIO_STATS=1`).

**T4. Validation sur une vraie META (avec l'utilisateur).**
Firmware 0.5.0 actuel : son continu, pas de lignes dupliquées, TBLEND en
streaming, menu « Ecran » sur SMOOTH sans freeze, Picomon (musique qui
boucle).  Puis un build `-DMETA_LCD_12BPP=1` : couleurs correctes,
fluidité.  Acceptation : retours notés dans `docs/META_STATUS_2026-09-30.md`
avec le label « mesuré sur console ».

**T5. Décider `META_LCD_12BPP` par défaut.**
Les estimations sont favorables (§3).  Si T4 confirme les couleurs, passer
le défaut à 1 dans `gbl_profile.h`, mettre à jour `targets/meta/README.md`.

### P1 — performance runtime (mesurer avec `EMU_PROF`)

Méthode commune : construire le projet (`pio run -d out/<jeu>`), copier
`firmware.bin` dans un dossier carte, lancer
`EMU_NOPACE=1 EMU_PROF=p.txt EMU_AUDIO_STATS=1 ./tools/meta-emu-sdl/meta_emu
<carte>/<NOM>.BIN --frames 3600`, puis
`tools/meta-emu-sdl/prof_report.py p.txt <firmware.elf>`.  Comparer A/B dans
le même run (voir le script `ab.py` décrit au §6).

**T6. File audio du runtime trop juste.**
`meta_audio.cpp` : LATENCY 256 éch. (11,6 ms), RING 768.  Les frames lourdes
(>21 ms) vident la file.  Essayer LATENCY/RING plus grands (RAM !) ou une
relance sans trou.  Acceptation : relances/min Mario Land et Zelda en
baisse, RAM libre ≥ 2 Ko.

**T7. Zelda : boucle d'attente interprétée.**
≈ 45 % du CPU dans l'interpréteur (`H_LDH_LD` + `H_ALUN_AND` + `H_JRNZ`).
Identifier l'adresse GB (trace runtime ou `gbl_interp` instrumenté), puis
soit la couvrir par les profils de compilation (`roms/gb/profiles`), soit
ajouter une détection de boucle d'attente STAT/LY (sauter au prochain
événement PPU).  Acceptation : CPU libre de Zelda > 25 %, `run_hash`
identique en `--meta-precise` vs `--interp` (règle AGENTS.md).

**T8. APU en Thumb-1.**
`gbl_apu_render` reste à 12,7 % sur Mario Land.  Écrire la boucle de
mixage en assembleur (comme `meta_display_asm.S`), garder le C comme
référence hôte et un test d'équivalence (sortie octet par octet sur des
états aléatoires).  Acceptation : APU < 8 %, test vert.

**T9. Attente active du limiteur de frames.**
`meta_micros` ≈ 30 % (temps libre en spin).  Remplacer par WFI entre deux
SysTick (économie d'énergie) si la latence reste bonne.  Vérifier que
l'émulateur gère WFI (aujourd'hui no-op : il faudra avancer le temps jusqu'à
la prochaine IRQ).

### P1 — fidélité de l'émulateur

**T10. Horloge CPU 72 MHz.**
Le menu « Processeur » passe le CPU à 72 MHz (FDPLL, RWS=2, SysTick LOAD
72000-1).  L'émulateur suppose 48 MHz partout et ignore SYST_RVR.  Modéliser
GCLK0 source DPLL (ratio), RWS, SYST_RVR/CVR réels.  Acceptation : un jeu
réglé à 72 MHz tourne 1,5x plus de cycles par seconde émulée, millis exact.

**T11. Latences de la carte SD.**
Les réponses SD sont instantanées (pas de Nac, pas de busy d'écriture).
Ajouter une latence de lecture/écriture réaliste et configurable.

**T12. Points ouverts du handoff.**
INTPEND (bit PEND = flag & enable) ; contention bus DMA/CPU non modélisée ;
lectures flash du DMAC hors cache ; SERCOM5, WDT, ADC (aléatoire).

**T13. Tests CTest hors META.**
Comprendre `accuracy_runner_exit_policy` et
`crystal_challenge_rules_are_bounded_and_deterministic` (environnement ?
catalogue de ROMs absent ?) ; confirmer par `git stash` qu'ils échouaient
déjà.

**T14. (fait) Pokitto Zelda écran bleu.**
Ce n'était que l'absence de carte : avec `LEGENDOF.GB` + `LEGENDOF.SAV`
dans un dossier passé en carte, le jeu démarre (ancien et nouveau cœur).

### P2 — outillage et idées

**T15. Script de non-régression de l'émulateur.**
`tools/meta-emu-sdl/regress.sh` : les 15 firmwares du §2, 1 800 frames,
capture + dernière ligne `[audio]` + compte de `pc fou`, planche contact
PNG ; seuils (0 pc fou, famine ≤ 1 %).  À lancer après tout changement de
`meta_emu.c`.

**T16. Vitesse de l'émulateur (wasm).**
Pistes : pré-décodage par bloc de base, `ld32/st32` sans test de cible
(pointeurs de fonction par cible), éviter `fetchHalf` pour BL.  Garder
`step` appelé par pointeur sous Emscripten.  Mesure : harnais Node
(`emcc -DEMU_NODE_HEADLESS … -sNODERAWFS=1`, `node x.js fw.bin /x 1800`).

**T17. ST7735 : idées d'économie SPI.**
Défilement matériel (VSCRDEF 0x33 / VSCRSADD 0x37) pour les jeux à scroll
vertical ; table RGBSET (0x2D) en 12 bpp pour changer de palette sans
renvoyer l'image.

**T18. SD à 25 MHz.**
`META_SD_SPI_HZ` à 24-25 MHz sur console (les cartes l'acceptent en SPI),
mesure du streaming (Zelda, Donkey Kong Land).

**T19. Docs.**
Mettre à jour `docs/META_STATUS_2026-09-30.md` (état, labels de preuve) et
`AGENTS.md` si le workflow change (profileur, régression émulateur).

## 6. Recettes utiles

- Reconvertir un jeu seul avec ses profils :
  `NO_ICONS=1 SESSIONS_DIR=$PWD/roms/gb/profiles ./compile.sh <dossier_roms> <dossier_sortie>`
  (le projet et l'ELF vont dans `out/<préfixe>/`, relatif au CWD).
- Variante d'un projet : copier `out/<jeu>`, supprimer `.pio`, éditer
  `platformio.ini` (`build_flags`) ou `src/`, `pio run -d <copie>`.
- Menu d'options dans l'émulateur : `EMU_INPUT="300:H:45,380:D:3,…"`
  (HOME tenu, puis flèches/A/B).
- Comparaison A/B : boucle Python qui lance chaque variante avec
  `EMU_PROF`, agrège par fonction (`arm-none-eabi-nm -n`) et additionne les
  `relances=` des lignes `[audio]`.
