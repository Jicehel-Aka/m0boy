# Profiling : sauvegarder les découvertes avant la fin du segment

L'explorateur gardait uniquement l'état final d'un segment qui avait trouvé
du code ou des données nouveaux. Une mort ou un retour au titre entre la
découverte et cette sauvegarde pouvait faire perdre un point de reprise utile.

Sur Game Boy et Game Gear/Master System, `m0x_observe` conserve désormais
la dernière découverte du segment, à la première frontière de frame sûre.
La Game Boy utilise son point sûr entre instructions ; la Game Gear rend
la main au harnais après la synchronisation des périphériques. Les états
ne sont pas copiés depuis les callbacks de périphériques.

Une seule sauvegarde temporaire est nécessaire, réutilisée quand le stock
est plein. Le stock de 48 états, le redémarrage périodique depuis le début,
les durées de segments et les compteurs globaux de profiling restent les
mêmes. Le harnais SG-1000 conserve l'observation en fin de segment.
Les profils existants sont conservés ; `AUTO_REDO=1` permet de refaire les
sessions avec cette stratégie.

## Comparaison sur l'hôte

Référence : sources `ce83a27`. Les deux harnais sont liés aux mêmes
bibliothèques runtime reconstruites avec CMake/Ninja, compilation hôte `-O3`.
Chaque jeu est joué avec `--explore 24000`, graines 1 et 2, puis les
ensembles des deux profils sont fusionnés, comme pour la conversion.
Chaque variante joue exactement 26 428 et 25 013 frames : l'explorateur
termine le segment en cours après avoir atteint son budget.

| Jeu GG | Positions de code avant → après | Écart | Secteurs de données avant → après |
|---|---:|---:|---:|
| Ristar | 10 282 → 11 524 | +12,1 % | 307 → 363 |
| Sonic Triple Trouble | 11 743 → 12 884 | +9,7 % | 421 → 424 |
| Shinobi | 9 972 → 9 894 | −0,8 % | 233 → 229 |
| Columns | 4 126 → 3 997 | −3,1 % | 45 → 45 |
| Somme par jeu | 36 123 → 38 299 | +6,0 % | 1 006 → 1 061 |

Le total des secteurs distincts augmente de 5,5 %. Ce petit échantillon
montre un compromis de parcours, pas un gain garanti pour chaque jeu ou
chaque graine. Il ne mesure ni le gain de vitesse des firmwares ni une
amélioration de compatibilité. Aucun résultat matériel n'est revendiqué.
Les profils bruts et journaux restent dans `logs/profile-check/`, ignoré
par Git ; les exécutables de comparaison sont dans `out/profile-check/`.

## Vérification

- Construction complète avec CMake/Ninja ; `ctest --test-dir build
  --output-on-failure -j 2` : 47/47 tests réussis.
- Test synthétique : conserver la dernière découverte malgré une mort,
  remplacer les états quand le stock est plein, préserver le démarrage,
  ignorer les segments sans découverte et respecter le budget.
- Tests e2e Game Boy et GG/SMS : deux explorations de même graine doivent
  produire les mêmes hashes et le même fichier de couverture.
- Sur le premier segment de Ristar, sans reprise d'état, ancien et nouveau
  harnais produisent exactement la même sortie et le même profil complet.

Cette modification concerne les outils hôtes de profiling ; elle ne modifie
pas le runtime des firmwares et ne change pas les versions par console.
