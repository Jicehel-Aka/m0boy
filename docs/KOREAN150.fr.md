# Super Game 150 : mapper à six fenêtres de 8 Kio

La cartouche de 2 Mio identifiée par le CRC32 `1b8956d1` est reconnue comme
`korean150` par `--info`, le générateur et le harnais. La détection ne lit la
cartouche entière que sur PC ; le firmware fixe `GG_MAPPER=2` à la génération.

Les registres FFFC (bits 7–4) et FFF3 (bits 5–0) commandent les six fenêtres
de 8 Kio sous C000. Tous les modes, le choix pair/impair, les pages BIOS
80/81 et les pages de remplissage FE/FF suivent les relevés matériels du
[dump de la cartouche par bsittler](https://www.smspower.org/forums/19752-DumpingSuperGame150).
Les autres registres bancaires Sega ne déplacent pas ce mapper. Les octets
des registres sont aussi écrits dans la RAM de travail, comme les autres
écritures sous FFFF.

La recompilation utilise des localisations `page_8k << 16 | adresse_cpu`,
des régions qui restent dans leur fenêtre et des gardes de page à l'entrée
et après les appels. Une écriture peut déplacer simultanément le code
courant, les données et les vecteurs d'interruption : après chaque
instruction qui écrit en mémoire (hors pile : PUSH, CALL, RST, EX (SP),HL),
le code natif synchronise, et le répartiteur reprend sous le nouveau mapping.
Les écritures statiques sur FFF3 et FFFC sortent de la région. Le reste est
le mode rapide des autres jeux (synchronisation en tête de boucle) ; le mode
précis (`--meta-precise`, une synchronisation avant chaque instruction) reste
disponible. Le mode précis coûtait 99 octets de flash par instruction
compilée (paramètre du générateur ; 76 mesurés dans `sg_gen.o`) contre 50 ici :
1896 instructions compilées au lieu de 1084 dans la même flash. Les autres
cartouches ne passent pas par ce chemin : le code généré de Ristar (GG) et de
Sonic (SMS) est identique octet pour octet avant et après.
Le stockage physique conserve ses secteurs de 512 octets et ses pages
résidentes de 16 Kio ; une fenêtre de 8 Kio peut viser l'une ou l'autre moitié.

Les profils portent `# bank-shift 13`. Les anciens profils Sega de la même
ROM ne décrivent pas les bonnes localisations : le générateur les refuse,
et `compile.sh` refait les sessions automatiques de cette cartouche. Les
profils manuels doivent également être rejoués avec le nouveau harnais.
Les états sérialisent les six fenêtres et leurs registres ; le chargement
reconstruit les pointeurs vers la ROM.

Validation automatisée : toutes les valeurs des deux registres contre une
table indépendante des huit modes ; lectures aux deux bords de chaque
fenêtre, vecteurs, registres Sega ignorés et aller-retour d'état. Une ROM
entièrement synthétique vérifie les écritures indirectes depuis la RAM,
les appels natifs dans chaque fenêtre et l'égalité précise avec
l'interpréteur en projets desktop et ESPboy (ROM externe incluse).

Cette validation porte sur le mapper et la recompilation. Elle ne constitue
pas une validation des 150 jeux, des modes vidéo hérités ou du matériel
ESPboy. Les essais de la cartouche réelle sont consignés avec les artefacts
dans `logs/espboy-request/`.

Validation du 23 octobre 2026 : les 59 tests CTest passent. Sur la cartouche
locale, une séquence sélectionnant Teddy Boy donne le même `run_hash`
`b3d76238` après 1800 images dans le harnais générique, le projet ESPboy
interprété et son code natif. Le firmware Xtensa (7046 instructions
compilées) tient avec 77444/81920 octets de RAM et 867955/1044464 octets de
flash applicative ; pile de continuation 10240 octets. Il lance Teddy Boy
dans gm0. Les captures du même firmware après 1200 images d'émulateur sont
identiques sous Linux et sous Windows/Wine. Ce sont des essais émulés,
sans mesure de performance sur ESPboy physique.

Le choix automatique du budget de code utilise maintenant aussi la chaîne
Xtensa de `PLATFORMIO_CORE_DIR` lorsqu'il est défini ; sans cela, une
installation isolée ne retrouvait pas `xtensa-lx106-elf-size` et abandonnait
après le premier dépassement de flash au lieu de réajuster le code.

## Jeux MSX : modes TMS9918 du VDP

Les tables de pages ci-dessus ne sont pas en cause quand seul le premier jeu
(Teddy Boy) s'affiche : les jeux MSX de la cartouche (la grande majorité des
entrées) programment le VDP en Graphics I/II (registre 0 sans M4, `02` ; Teddy
Boy est en mode 4, `26`), que le VDP Game Gear/Master System du runtime ne
dessinait pas : écran de tuiles lues comme du mode 4. Le VDP de la Master
System dessine ces modes comme un TMS9918, avec sa palette fixe (les seize
couleurs TMS converties en `--BBGGRR`, table dans `gg_vdp.c`).

`gg_vdp.c` ajoute, pour ce seul mapper (`GG_KOREAN150` est une constante dans
les projets générés, le code disparaît des autres firmwares : le firmware de
Sonic Blast GG est identique octet pour octet), Graphics I et II, le texte et
le multicolore, les sprites 8x8 et 16x16 agrandis ou non, la limite de quatre
sprites par ligne avec le drapeau du cinquième, et la coïncidence. Les tuiles
sans sprite sont filtrées de 256 à 160 colonnes directement depuis leur motif
et leurs deux couleurs ; les groupes sous un sprite, le texte et le
multicolore passent par des indices de couleur et le même filtre. Les deux
chemins donnent les mêmes images (comparées pixel pour pixel sur 256 états de
lancement du menu, soit tous les couples mode/banque des jeux MSX).

Validation : `test_gg_runtime` (tuile de quatre pixels blancs et quatre noirs
en Graphics I et II, écran éteint) ; relevé d'images du harnais pour les jeux
MSX de la cartouche (Knightmare, Goonies, Wonder Boy, Galaxian, Hyper Rally,
Mr. Chin, etc. s'affichent). Non vérifié : les jeux sur console (la META n'a
pas encore tourné avec ce rendu), les couleurs contre un matériel réel (la
table des couleurs vient de la documentation de la communauté, pas d'une
mesure), le son des jeux MSX (PSG AY ou SCC éventuels, non pris en charge : le
runtime n'émule que le SN76489), et le débit : un jeu MSX tourne presque
entièrement dans l'interpréteur tant qu'aucune session de jeu ne l'a profilé.

