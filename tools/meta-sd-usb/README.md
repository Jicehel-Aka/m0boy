# META SD USB

Firmware autonome pour la Gamebuino META (SAMD21G18A). Il expose **toute la
carte microSD comme un lecteur USB Mass Storage** : lecture et écriture de
secteurs de 512 octets. Le PC gère le système de fichiers. Le script
`meta_sd.py` donne à un agent une interface de transfert avec réponses JSON.
Un `put` de fichier `.zip` extrait automatiquement son contenu à la racine
de la carte, ou dans le dossier choisi ; `--raw` copie le ZIP sans extraction.

Le firmware ne monte aucun FAT lui-même. La décompression ZIP s'exécute sur
le PC par le script, après montage du lecteur USB. Une copie directe d'un ZIP
avec l'explorateur de fichiers laisse le ZIP tel quel sur la carte. Le script
accepte les ZIP `stored` et `deflate` de Python, vérifie leurs CRC, refuse
les chemins sortant de la carte et les liens symboliques.

## Compiler et démarrer

Depuis ce dossier :

```sh
pio run
```

Le résultat est `.pio/build/meta/firmware.bin`. Une copie prête à utiliser
est disponible dans `../../out/meta-sd-usb/META_SD_USB.BIN` depuis la racine
du dépôt parent. Cette copie est générée localement, exclue de Git selon les
règles du dépôt parent. Copiez `META_SD_USB.BIN` sur la carte avec un lecteur
microSD, lancez-le depuis le loader Gamebuino, puis branchez le port USB de
la console à l'ordinateur. L'ordinateur doit voir un périphérique de stockage
amovible « META SD Bridge ». La carte doit déjà contenir un système de
fichiers reconnu par l'ordinateur (habituellement FAT32). Une carte absente
au démarrage est signalée comme support indisponible ; insérez-la puis
redémarrez le firmware.

Le firmware utilise l'horloge nominale à 48 MHz, le bus SD à 12 MHz et le
pilote USB matériel du SAMD21. Il reste indépendant du runtime des jeux.

## Transferts pilotables par agent

Montez le lecteur USB sur le PC, puis indiquez son point de montage. Les
commandes rendent du JSON sur stdout et un code de sortie non nul en cas
d'erreur. `META_SD_MOUNT` peut remplacer `--root`.

```sh
python3 meta_sd.py --root /media/$USER/META ls
python3 meta_sd.py --root /media/$USER/META put jeu.zip
python3 meta_sd.py --root /media/$USER/META put jeu.zip JEUX/MonJeu
python3 meta_sd.py --root /media/$USER/META put notes.txt DOCUMENTS
python3 meta_sd.py --root /media/$USER/META get JEUX/MonJeu/README.txt ./README.txt
python3 meta_sd.py --root /media/$USER/META mkdir SAUVEGARDES
python3 meta_sd.py --root /media/$USER/META rm SAUVEGARDES/ancien.sav
```

`put` accepte aussi un dossier et le copie récursivement. `unzip archive.zip
[dossier]` explicite l'extraction. Les chemins côté carte sont relatifs à la
racine ; `.` désigne celle-ci. Pour copier le ZIP lui-même : `put jeu.zip .
--raw`.

## Quitter le mode USB

Trois façons de rendre la console au loader Gamebuino (le firmware se détache
du bus puis déclenche un reset watchdog, que le bootloader traduit en
lancement du loader depuis la carte) :

- sur la console, **HOME + MENU** (la paire Select/Start de la META) tenus
  une demi-seconde ;
- sur le PC, `python3 meta_sd.py exit` : démonte le volume (en forçant si une
  fenêtre de gestionnaire de fichiers le retient) et l'éjecte via udisks,
  sans privilège particulier. Le firmware prend la commande SCSI START STOP
  UNIT avec LOEJ comme demande de sortie ; udisks répond « busy » pendant
  quelques secondes après un démontage, le script réessaie ;
- éjecter le volume depuis le gestionnaire de fichiers.

La sortie n'attend pas la fin d'un transfert en cours : elle n'a lieu qu'entre
deux commandes.

Après démontage du volume, `python3 meta_sd.py bootloader` demande au firmware
de redémarrer en mode sam-ba pour un nouveau téléversement. Cette requête USB
nécessite l'accès en écriture au périphérique USB (règle udev ou exécution
avec privilèges). Le double appui sur RESET reste une solution sans règle udev.

Éjectez/démontez proprement le volume sur l'ordinateur avant de débrancher
la console ou de retourner au loader. Ne montez pas simultanément la même
carte dans un autre lecteur. Les fichiers extraits sont écrits dans des
fichiers temporaires du même dossier, puis remplacés individuellement ; une
archive interrompue peut donc laisser les fichiers précédents en place.

## Coût dans m0boy

Dans les firmwares de jeu de m0boy (0.10.39), le mode lecteur USB du menu
(`META_USB_MODE`, `meta_usb_msc.c`) réutilise le pilote USB, l'init SD et
l'horloge du jeu : il ne reprend ni le démarrage, ni le chargeur d'horloges de
ce firmware autonome. Mesuré avec `arm-none-eabi-size` (flash = text + data,
RAM statique = data + bss) sur le même projet META (Super Game 150, Master
System), seul `META_USB_MODE` changeant :

| `META_USB_MODE` | Flash | RAM statique | Écart avec 0 |
|---|---|---|---|
| 0, sans USB | 239 248 o | 31 424 o | |
| 1, port série de débogage seul | 241 828 o | 31 844 o | +2 580 o, +420 o |
| 2, lecteur SD seul | 243 088 o | 31 876 o | +3 840 o, +452 o |
| 3, série et lecteur SD (défaut) | 245 728 o | 31 908 o | +6 480 o, +484 o |

Le lecteur SD ajouté au série coûte donc **environ 3,9 Ko de flash et 64 octets
de RAM** ; seul, il coûte 3,8 Ko de flash et 452 octets de RAM (le pilote USB
est alors à lui seul). Le firmware autonome de ce dossier occupe 9 692 octets
de flash et 1 012 octets de RAM, démarrage et horloges compris. La flash des
jeux est pleine à 99,9 % : ces 3,8 à 3,9 Ko représentent environ 75 instructions
Z80 compilées en moins par jeu (à 52 octets chacune). Une mesure, un projet,
une cible : le chiffre varie un peu d'un jeu à l'autre.

## Vérification et portée

```sh
python3 -m unittest discover -s tests -v
pio run
```

Le build local utilise 9 324 octets de flash et 1 004 octets de RAM statique.
Les tests Python couvrent l'extraction à la racine et dans un dossier, le
rejet de `../` et des liens symboliques. Sur une META physique (23 octobre
2026), Linux a énuméré le périphérique et sa carte de 7,3 Gio, puis monté sa
partition FAT32. Le CLI a extrait un ZIP compressé contenant deux fichiers ;
après démontage et remontage, les 4 096 octets du fichier de contrôle ont
été relus à l'identique. Les fichiers de contrôle ont été retirés. Ce test
valide un transfert court, pas une longue session ou toutes les cartes SD.
Le système de fichiers du PC doit être éjecté proprement après toute
écriture. Le protocole prend en charge
INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY, READ/WRITE(10), MODE
SENSE et les commandes de synchronisation usuelles ; les cartes qui exigent
exFAT dépendent du support exFAT de l'ordinateur.

Les fichiers `src/startup.c`, `include/meta_hw.h`, `gamebuino_meta.ld` et
`boards/gamebuino_meta.json` proviennent du dépôt m0boy (pilote META bare
metal). Ils sont recopiés ici pour rendre ce projet Git autonome.
