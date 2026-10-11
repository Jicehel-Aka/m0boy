# CPU, console d’origine et destination

La sélection d’un jeu comporte désormais trois identités indépendantes :

| Axe | Game Boy | SG-1000 |
| --- | --- | --- |
| Console d’origine (`--system`) | `gameboy` | `sg1000` |
| CPU requis (`--cpu`, contrôle facultatif) | `sm83` | `z80` |
| Destination (`--target`) | `desktop`, `meta`, `pokitto` | Aucune génération disponible pour le moment |
| État actuel | Pipeline existant | Chargement, inspection et désassemblage Z80 des cartouches sans mapper |

`--target sg1000` n’est pas une destination : la SG-1000 est la machine
dont on traduit le programme. META et Pokitto restent les machines sur
lesquelles les firmwares Game Boy s’exécutent.

## Organisation

- `recompiler/include/m0boy/memory.h` : interface `ByteReader`. Un décodeur
  reçoit des octets et une adresse, sans connaître le format de cartouche.
- `recompiler/include/m0boy/system.h`, `recompiler/src/system.cpp` : registre
  des CPU, des consoles d’origine et de leurs capacités de génération.
- `recompiler/include/m0boy/cpu/encoding.h` : codes de registres, champs
  `x/y/z/p/q`, lecture little-endian et calcul des branches relatives communs.
- `recompiler/src/cpu/z80/`, `include/m0boy/cpu/z80/` : désassemblage Z80,
  préfixes `CB/ED/DD/FD`, registres indexés, timings et métadonnées de contrôle
  de flux. La bibliothèque `m0boy_cpu_z80` ne dépend pas des cartouches.
- `recompiler/src/cpu/sm83/`, `include/recompiler/cpu/sm83/` : décodage SM83.
  La bibliothèque CMake `m0boy_cpu_sm83` se construit sans le parseur Nintendo.
- `recompiler/src/system/gameboy/` : cartouche Game Boy, analyse des banques,
  suivi des mappers et balayage du code par banque.
- `recompiler/src/system/sg1000/`, `include/m0boy/sg1000.h` : cartouche SG-1000.
- `m0boy_frontends` : chargement et métadonnées des consoles d’origine.
- `gbrecomp_core` : analyse, IR et générateurs Game Boy existants.
- `targets/meta/` : runtime Game Boy et pilotes des destinations existantes.

Les anciens includes `recompiler/rom.h` et `recompiler/decoder.h`, le namespace
`gbrecomp` et l’exécutable `gbrecomp` restent utilisables. Le nouvel exécutable
`m0recomp` utilise exactement la même CLI. `compile.sh` et le frontend Node
continuent à utiliser le pipeline Game Boy.

L’IR, les optimisations et les générateurs existants gardent leur sémantique
SM83/Game Boy. Ils ne constituent pas encore un IR commun au Z80. Le runtime
`gbl` reste propre à la Game Boy ; aucun périphérique SG-1000 n’y est ajouté.

## Utilisation

```bash
build/bin/m0recomp --list-systems
build/bin/m0recomp jeu.sg --info
build/bin/m0recomp jeu.sg --disasm --limit 32
build/bin/m0recomp jeu.bin --system sg1000 --cpu z80 --info
build/bin/m0recomp jeu.gb --system gameboy --target meta -o output/jeu
```

Le mode `auto` reconnaît `.sg` et `.gb/.gbc/.sgb`, sans distinction de casse.
Les extensions inconnues et les dossiers conservent le choix Game Boy
historique. Pour un fichier `.bin` SG-1000, il faut donc indiquer `--system
sg1000`. `.sgb` désigne toujours une cartouche Game Boy, pas une SG-1000.
La sélection du CPU contrôle la cohérence avec la console, elle ne change
pas son matériel.

Une cartouche SG-1000 n’a pas besoin d’un en-tête Nintendo. Le premier profil
accepte de 1 à 48 Kio de ROM brute, sans mapper ni RAM de cartouche. Les tailles
supérieures sont refusées avec un diagnostic explicite. Le lecteur statique
expose les octets du fichier à partir de l’adresse zéro ; il retourne `0xff`
hors du fichier et pour toute banque autre que zéro. Ce lecteur ne simule
pas les miroirs électriques de ROM ou la RAM de la console.

Le profil déclare le reset Z80 à `0000` et 1 Kio de RAM système. La
[carte mémoire SG-1000](https://www.smspower.org/Development/MemoryMap)
place la cartouche dans `0000–BFFF` et la RAM dans `C000–C3FF`, avec des
miroirs jusqu’à `FFFF`. Les extensions et cartouches qui remplacent cette
carte mémoire demanderont leurs propres profils.

L’inspection et le désassemblage SG-1000 fonctionnent. Le désassemblage est
un balayage linéaire : des données peuvent apparaître comme des instructions.
`--limit` limite le nombre d’instructions affichées. L’analyse de code atteignable
et la génération sont explicitement refusées avant tout passage dans le
pipeline Game Boy. Aucun firmware SG-1000 n’est produit à cette étape.

Le SM83 est apparenté au Z80, mais n’est pas un Z80 interchangeable. Les
codes de registres et certains encodages sont communs ; les décodeurs les
partagent sans partager les drapeaux ni les timings. Les préfixes indexés,
les registres alternatifs et les ports d’E/S restent propres au Z80 ; les
instructions `LDH`, `STOP` et `SWAP` restent propres au SM83. Le décodeur Z80
marque les encodages non documentés, dont `SLL` et les accès `IXH/IYH`. Il
fournit des métadonnées de désassemblage, pas des instructions exécutables.

## Suite de l’implémentation SG-1000

1. Compléter la validation du décodeur Z80 contre le
   [manuel Zilog](https://www.zilog.com/docs/z80/z80cpu_um.pdf), puis ajouter
   ses opérations exécutables et ses drapeaux. Le désassemblage est en place.
2. Ajouter l’analyse du contrôle de flux Z80 et une représentation de ses
   instructions adaptée à la génération native.
3. Ajouter un runtime SG-1000 : bus mémoire et ports, interruptions, vidéo,
   audio et contrôleurs. La cartouche reste un composant de la console.
4. Brancher les destinations META/Pokitto et un harness hôte sur ce runtime,
   puis annoncer les capacités dans le registre après validation.

Les nouveaux tests utilisent exclusivement des données synthétiques. Ils
vérifient les limites de cartouche, la sélection de la console avant les
émetteurs Game Boy, les familles de préfixes Z80 et le décodage SM83 sans
dépendance au parseur Game Boy. Les comparaisons des deux CPU vérifient
explicitement les différences de timing et `SWAP`/`SLL`.
