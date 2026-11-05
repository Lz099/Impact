# Impact

Un projet Unreal Engine 5.7 en C++ où j'essaie d'obtenir des réactions physiques crédibles quand un personnage se fait toucher par une balle. C'est un banc d'essai, pas un jeu complet.

Je m'inspire d'Euphoria sans prétendre l'égaler : le ragdoll est un hybride entre des animations et de la simulation. J'ai aussi une version entièrement physique (`ImpactActiveBodyComponent`), mais elle est désactivée par défaut (option `-ImpactCorpsActif`).

## Ce qu'il y a dedans

Le code est dans `Source/Impact`, environ 23 000 lignes.

- Le ragdoll actif (`ImpactReactionComponent`) : un `UPhysicalAnimationComponent` et des moteurs angulaires sur les contraintes, avec une force musculaire qui change selon l'état du personnage (debout, chute, au sol...).
- La balistique par zone (`ImpactAnatomy`, `ImpactBodyComponent`) : chaque balle applique une impulsion à l'endroit touché. Gilets et casque absorbent une partie de l'énergie avant l'anatomie, avec des valeurs de jeu, pas de vraies normes.
- La physiologie (`ImpactPhysiology`) : volume sanguin, hémorragie interne et externe, coagulation, respiration, rythme cardiaque, jusqu'à la perte de connaissance et l'arrêt cardiaque.
- Les soldats IA (`ImpactSoldierBrainComponent`) : une machine à états écrite à la main (`Idle`, `Engage`, `SeekCover`, `InCover`, `Peek`, `Down`), des abris choisis par line trace et des déplacements sur NavMesh. Pas d'arbre de comportement.
- Les armes, les munitions et l'équipement.
- Dans `Tools/`, des scripts Python et PowerShell pour importer les animations, faire le retargeting, construire le NavMesh et lancer les bancs d'essai. `ArtSource/` contient mes sources Blender (équipements et animations au sol).

## Tests

Il y a 20 tests d'automatisation dans `Source/Impact/Tests` : physiologie, munitions, accessoires, armures et vitesse de déplacement. Ils ne touchent ni au ragdoll, ni aux animations, ni à l'IA. Ces parties-là, je les valide avec des bancs d'essai : le jeu se lance, joue un scénario (`-ImpactTest=<nom>`) et se ferme après un délai (`-ImpactQuit=<secondes>`).

```powershell
UnrealEditor-Cmd.exe Impact.uproject -ExecCmds="Automation RunTests Impact" -unattended -nopause -nosplash -NullRHI -testexit="Automation Test Queue Empty"
```

## Compiler

Windows, Unreal Engine 5.7 et Visual Studio avec la chaîne C++.

Le dépôt ne se lance pas tel quel. Le projet repose sur [ALS Refactored](https://github.com/Sixze/ALS-Refactored) (Sixze, MIT) que j'utilise avec quelques modifications locales, et je ne l'ai pas inclus. Je n'ai pas non plus mis les animations Mixamo ni le personnage que j'ai utilisés pendant le développement, ni le pack d'armes. `Content/` ne contient que mes équipements et mes matériaux.

## Ce qui manque

Pas de mesure de performance, pas de test en multijoueur ni en build empaqueté, pas de vidéo pour l'instant.

Licence MIT pour mon code, voir `THIRD_PARTY_NOTICES.md` pour le reste. Unreal Engine est une marque d'Epic Games.
