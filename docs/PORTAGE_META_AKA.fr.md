# Reprendre un jeu Gamebuino META sur l'AKA (guide)

Un jeu META est un binaire `.bin` pour SAMD21 (Cortex-M0+). `m0recomp` en traduit
le code en C, qui tourne sur l'ESP32-S3 de l'AKA au-dessus d'un modèle de carte
SAMD21 (TC4/TC5, DMAC, SERCOM4 + ST7735, DAC, PORT). Aucun émulateur dans le
firmware, aucune modification du jeu. On n'a besoin que du `.bin` du jeu.

Exemples : `lapinou.bin` (231 Ko, sources `meta-lapinou`) et
`CatsAndCoinsDemo.bin` (104 Ko, base 0x4000).

## 1. Le plus simple : le workflow GitHub

1. Déposer le `.bin` dans le dépôt (par exemple `tools/aka/samples/Jeu.bin`) et le pousser sur le fork.
2. GitHub > Actions > **Firmware AKA depuis un jeu META** > Run workflow.
3. Remplir : `rom` = chemin du `.bin` ; laisser le reste par défaut au début (voir §3).
4. Une dizaine de minutes plus tard, télécharger l'artefact `firmware-aka-<Jeu>-<mode>`.

En ligne de commande (même résultat) :

```bash
gh workflow run aka-meta-firmware.yml -R Jicehel-Aka/m0boy --ref main \
   -f rom=tools/aka/samples/Jeu.bin -f mode=hybride -f opt=O2 -f iram=0
gh run watch -R Jicehel-Aka/m0boy
gh run download -R Jicehel-Aka/m0boy -n firmware-aka-Jeu-hybride
```

Ce que fait le workflow, dans l'ordre :

| Étape | Rôle |
|---|---|
| Couverture | joue `parties` parties aléatoires sur PC (`m0_headless --random N`) et note chaque instruction rencontrée : seules ces instructions seront traduites |
| Projet AKA | `m0recomp <rom> --system meta --target aka --coverage all.cov` |
| Pages chaudes | classe les pages de 1 Ko de code par cycles consommés |
| Compilation | PlatformIO, avec les options choisies (`mode`, `opt`, `iram`) |
| Paquet | artefact prêt à flasher (voir §2) |

## 2. Flasher le paquet

L'artefact contient exactement ce que contient le paquet de Lapinou :
`<Jeu>-aka-flash.bin` (image unique), `bootloader.bin`, `partitions.bin`,
`boot_app0.bin`, `firmware.bin`, `firmware.sym`, `FLASH.txt`, `README.md`, `meta.json`.

```bash
esptool.py --chip esp32s3 --port COM5 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 Jeu-aka-flash.bin
```

(Sous Windows : `python -m esptool ...`, port `COMx`.) Les commandes exactes sont dans `FLASH.txt`.

Le firmware écrit sur le port série USB (115200) les lignes décrites au §4.
`tools/aka/serial_console.pyw` les affiche. Attention : fermer la console
redémarre la console AKA (la ligne DTR/RTS de l'USB-Serial/JTAG du S3 la
réinitialise) ; ce n'est pas un plantage du jeu.

## 3. Réglages du workflow

| Entrée | Valeurs | Quand y toucher |
|---|---|---|
| `bgr` | oui (défaut) / non | rouge et bleu inversés à l'écran → inverser la case |
| `mode` | `hybride` / `natif` | `hybride` : interpréteur compact + modèle de carte en RAM interne (le plus rapide mesuré sur Cat's and Coins). `natif` : tout le code traduit, lu depuis la flash (sature le cache d'instructions de 16 Ko) |
| `opt` | `Os` / `O2` | `O2` avec `hybride` |
| `iram` | 0, 1, 2, … | nombre de pages de code traduit mises en RAM interne ; **0** = interpréteur seul. Le workflow redescend tout seul si ça ne tient pas |
| `pages` | liste hexa | forcer les pages en RAM interne (celles de la ligne `[prof]`) ; vide = choix par profil PC |
| `parties` | 8 (défaut) | plus de parties = meilleure couverture, plus long. Si le jeu se fige sur un écran rarement visité : augmenter |

Réglage de départ conseillé : `mode=hybride opt=O2 iram=0`.

## 4. Lire les mesures de la console

Une fois par seconde :

- `[perf]` : images par seconde, temps par image (émulation, LCD), Mcycles/s guest, `idle skipped` (attentes sautées).
- `[time]` : temps des chaînes natives, des accès machine, de l'interpréteur de secours.
- `[board]` : coût (µs, appels) de `sync`, lecture de statut, `tick`, `room`, `idle`, `loop`.
- `[rom]` : lectures de la ROM du jeu.
- `[fast]` : instructions guest exécutées par image, cycles ESP32 par instruction.
- `[prof]` : pages de code les plus chaudes (échantillonnage approximatif).

Règle de lecture : 60 images/s correspondent à environ 48 Mcycles/s guest. Le
jeu tourne à pleine vitesse quand `[perf]` affiche ce débit (les attentes
sautées comptent).

## 5. Si le jeu est lent

1. Essayer `mode=hybride opt=O2 iram=0`, puis `iram=4`, `8` (le workflow ajuste).
2. Lancer sur PC le profil du jeu : `m0_headless --frames 1200 --random 3 --profile p`
   (le fichier `p.hot` classe les instructions par cycles) et chercher les
   boucles qui dominent : doublage de pixels, copie de sprites, conversion de couleurs.
3. Ces boucles se remplacent par un noyau en C (`targets/m0plus/runtime/m0_native_kernels.h`) :
   le noyau reconnaît la boucle par ses instructions exactes, fait les
   itérations d'un coup et rend registres, cycles et écritures identiques.
   Règle de vérification : `m0_headless --frames 1200 --random 3` doit
   afficher le même `run_hash`/`image_hash`/`audio_hash` qu'avec `--interp`.

## 6. Limites connues

- La carte SD du jeu n'est pas modélisée (sauvegardes et fichiers du jeu indisponibles).
- Les écrans non vus pendant la couverture sont plus lents (interpréteur de secours).
- Validé sur PC (égalité des empreintes avec l'interpréteur de référence) ; la vitesse sur console dépend du jeu et reste à mesurer sur chaque titre.
- Seuls les jeux qui passent par les périphériques modélisés (TC4/TC5, DMAC, SERCOM4, DAC, PORT) fonctionnent.

## 7. Si on a aussi les sources (PlatformIO META)

Les sources du jeu (par exemple `meta-lapinou`) ne sont pas nécessaires :
`pio run -e gamebuino-meta` produit le `.bin` à donner à `m0recomp`
(`.pio/build/gamebuino-meta/firmware.bin`). Garder la même version de
bibliothèques que pour la version META, puis suivre le §1.
