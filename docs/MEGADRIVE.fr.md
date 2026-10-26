# Mega Drive : recompilation 68000 expérimentale

`m0recomp --system megadrive --target pc` génère un projet C jouable qui ne
dépend que de SDL2 pour sa fenêtre et ses entrées. Le CPU 68000, la mémoire,
le VDP et le rendu sont dans m0boy ; aucun cœur d'émulation externe n'est
chargé. Le générateur émet en C les opcodes couverts par un profil, puis les
exécute par le décodeur 68000 interne si une adresse n'est pas encore couverte.
Les ROMs restent dans `output/` et ne sont pas ajoutées au dépôt.

Pour jouer sur PC :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
./build/bin/m0recomp --system megadrive \
  'roms/md/Sonic The Hedgehog (USA, Europe).md' --target pc -o output/sonic_md
cmake -G Ninja -S output/sonic_md -B output/sonic_md/build
ninja -C output/sonic_md/build
output/sonic_md/build/md_pc
```

Pour fabriquer directement les exécutables Linux et Windows à partir de la
ROM locale, utilisez `tools/megadrive/build_pc_binaries.sh [ROM] [DOSSIER]`.
Le script entraîne la couverture avec une séquence Start, déplacement à droite
et saut, puis compile le code C couvert pour les deux plateformes :

```bash
tools/megadrive/build_pc_binaries.sh \
  'roms/md/Sonic The Hedgehog (USA, Europe).md' output/sonic1-pc
```

Les fichiers livrés sont `output/sonic1-pc/sonic1-linux` et
`output/sonic1-pc/sonic1.exe`, avec `SDL2.dll` à côté du `.exe`. Le binaire
Linux nécessite SDL2 installé sur le système. Le cross-build Windows demande
MinGW x86-64 et son paquet SDL2. Le script attend le recompileur déjà construit
dans `build/bin/m0recomp` (ou le chemin défini par `M0RECOMP`). Les programmes
intègrent la ROM passée au script ; gardez-les dans `output/`, conformément à
la règle qui exclut les exécutables dérivés de ROMs du dépôt et des archives.

Flèches pour bouger, X/Z/C pour A/B/C, Entrée pour Start, Échap pour quitter.
Une manette SDL2 utilise la croix ou le stick gauche, B/A/X pour A/B/C et Start.
Un joystick non reconnu comme manette utilise son chapeau ou ses axes et ses
boutons 0–3 pour A/B/C/Start. Le branchement à chaud est pris en charge.
L'image est sans filtre par défaut. F8 parcourt les filtres de gm0 (pixel,
DMG, LCD RGB, scanlines, grille, puis aucun),
F10 change l'échelle et F11 bascule en plein écran. Les filtres ne modifient
pas les captures ni le hachage de l'image native.

gm0 reste un programme voisin facultatif pour exécuter les firmwares des
consoles portables. Son interface SDL2 est intégrée à son émulation matérielle
et n'expose pas de bibliothèque de rendu ou de manette. En faire une dépendance
du projet Mega Drive généré importerait les machines META/Pokitto/ESPboy et
Zlib sans servir le cœur 68000. Le module SDL commun à toutes les cibles PC
reprend donc ses commandes et la géométrie 3× de ses filtres, avec SDL2 comme
seule dépendance.

Le PC exécute le Z80 de m0boy pour le pilote sonore, décode ses 8 KiB de
RAM, sa fenêtre ROM, le bus et les ports YM2612/PSG. Le 68000, le Z80, le
YM2612 et le PSG partagent l'horloge maître NTSC : avant chaque accès du
68000 à l'espace Z80, au PSG ou aux lignes BUSREQ/RESET, le Z80 et la
synthèse rattrapent l'instant de cet accès, et chaque écriture YM/PSG du Z80
prend effet à son cycle. Les échantillons DAC (le « SEGA » et les percussions
de Sonic) sont donc rendus à leur cadence. L'interruption du Z80 est tenue
une ligne à partir du début du blanking vertical. Le YM2612 est remis à zéro
avec le Z80, comme sur la console.

Le YM2612 produit un échantillon toutes les 144 horloges (53,3 kHz) :
générateur de phase 20 bits avec détune et multiplicateur, tables log-sinus
et exponentielle, enveloppes ADSR à compteur global (taux, échelle de touche,
niveau de sustain), SSG-EG (répétition, alternance, maintien, inversion),
huit algorithmes, rétroaction, mode spécial du canal 3 et mode CSM,
minuteries A/B et leurs drapeaux, drapeau busy (32 horloges internes après
une écriture de données), panoramique, DAC, LFO (AM, et PM approchée).
Chaque voie passe par le DAC 9 bits du YM2612 discret, avec son effet
d'échelle (un pas de plus aux niveaux positifs, le signe sur un côté muet).
Le PSG (SN76489 de Sega) compte à 223,7 kHz, avec son bruit à registre
16 bits. Le mélange est rééchantillonné à 44,1 kHz stéréo, puis le continu
est retiré ; `--wav FILE` l'enregistre même sans fenêtre (`--frames N`), et
`audio_hash` permet de comparer le code 68000 natif et l'interpréteur.

Le 68000 compte quatre cycles par mot lu ou écrit sur le bus (opcode,
extensions, opérandes, pile), plus les cycles internes des branchements,
modes indexés et prédécrémentés, multiplications, divisions, décalages et
interruptions. Ce décompte reste une approximation : préchargement et états
d'attente ne sont pas modélisés, et le DMA du VDP ne bloque pas le CPU.

Les filtres analogiques de sortie, qui changent selon le modèle de console,
ne sont pas reproduits. Le test `megadrive_m68k_end_to_end` mesure la
fréquence d'une voix FM et d'une voix PSG synthétiques ; le son de Sonic n'a
pas été comparé à une Mega Drive.

SDL2 est la seule dépendance du frontend PC. `--frames N --ppm FILE` permet
une capture sans fenêtre visible, et `--input FILE` rejoue des lignes
`trame masque` où haut=1, bas=2, gauche=4, droite=8, A=16, B=32, C=64 et
Start=128.

## Profil natif

La couverture des adresses 68000 exécutées se collecte avec `md_pc --interp
--coverage FILE`. Passez un ou plusieurs fichiers au générateur avec
`--coverage FILE` pour émettre ces adresses dans `src/md_full_code.c` comme
cases C contenant les opcodes ROM constants. Le programme fonctionne déjà
sans profil, via le décodeur interne. Les zones RAM exécutables restent dans
le décodeur. Les compteurs `native_hits` et `fallback_hits` rendent ce partage
visible ; `--interp` sert de référence pour comparer `run_hash` et `image_hash`.

Le test facultatif `megadrive_sonic_pc_native` utilise la ROM Sonic locale,
collecte le profil du titre et d'une séquence Start/jump, puis vérifie que la
génération C donne les mêmes états et pixels que l'interpréteur, sans fallback
sur cette séquence. Ne pas confondre ce résultat avec une compatibilité
générale : Sonic 1 est jouable dans ce frontend, mais les sprites,
plusieurs modes du VDP, la fidélité sonore et d'autres cartouches restent incomplets. Le
profil META conserve encore son ancien sous-ensemble restreint ; la jouabilité
sur matériel embarqué n'est pas établie.

Une ROM synthétique de test vérifie aussi l'en-tête, les vecteurs, l'exécution
du décodeur, la génération C et le rendu d'une tuile. La ROM libre de
[blastcomp](https://github.com/staruwos/blastcomp) a servi à valider la
reconnaissance du format, mais n'est pas incluse dans le dépôt.
