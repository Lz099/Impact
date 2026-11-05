# Impact — réactions physiques aux impacts (Unreal Engine 5.7, C++)

Module C++ pour Unreal Engine 5.7 qui explore des **réactions physiques crédibles aux tirs** sur des personnages : ragdoll actif piloté par moteurs d'articulations, mélange animation / simulation, impulsions balistiques par zone du corps, modèle physiologique et soldats IA. Le projet est un banc d'essai de gameplay, pas un jeu complet.

> Inspiré d'Euphoria, sans prétendre l'égaler : le mode ragdoll est un **hybride** animation + simulation. Une architecture entièrement physique existe (`ImpactActiveBodyComponent`) mais elle est désactivée par défaut (`-ImpactCorpsActif`).

## Ce que contient le dépôt

Environ 23 000 lignes de C++ dans `Source/Impact/` :

| Domaine | Fichiers principaux |
|---|---|
| Ragdoll actif et réactions | `ImpactReactionComponent` (`UPhysicalAnimationComponent` + moteurs angulaires des contraintes, force musculaire mélangée selon l'état, chutes mélangées animation/ragdoll), `ImpactBodyComponent`, `ImpactActiveBodyComponent` |
| Balistique par zone | `ImpactAnatomy` (zones et organes), `ImpactBodyComponent::ReceiveBullet`, impulsions `AddImpulseAtLocation`, `ImpactEquipmentComponent` (gilets et casque absorbent l'énergie avant l'anatomie ; valeurs de jeu, pas des normes certifiées) |
| Modèle physiologique | `ImpactPhysiology` : volume sanguin, hémorragie interne / externe, coagulation, respiration, rythme cardiaque, états de conscience (jusqu'à l'inconscience et l'arrêt cardiaque) |
| IA de soldats | `ImpactSoldierBrainComponent` : machine à états écrite à la main (`Idle`, `Engage`, `SeekCover`, `InCover`, `Peek`, `Down`), choix d'abris par *line trace* (ray-casting), déplacement via NavMesh (`ProjectPointToNavigation`, `FindPathToLocationSynchronously`). Pas d'arbre de comportement ni d'EQS. |
| Armes et équipement | `ImpactWeaponComponent`, `ImpactAmmoComponent`, `ImpactNpcShooterComponent`, `ImpactEquipment*`, `ImpactArmorRules` |
| Bancs d'essai | `ImpactGameplaySubsystem` (une vingtaine de scénarios `-ImpactTest=<nom>`), `ImpactAutoCapture`, `Tools/banc_*.ps1` |
| Outils d'import | `Tools/*.py` (retargeting, import d'animations, construction du NavMesh), `ArtSource/` (sources Blender originales) |

## Tests

- **20 tests d'automatisation** (`Source/Impact/Tests`) : physiologie (8), munitions (6), accessoires (3), armures (2), déplacement (1).
- Ils couvrent la logique unitaire. **Le ragdoll, le mélange d'animations et l'IA ne sont pas testés automatiquement** : ils sont validés par des bancs d'essai en jeu (fenêtre automatique avec captures), pas par des tests unitaires.

```powershell
UnrealEditor-Cmd.exe Impact.uproject -ExecCmds="Automation RunTests Impact" -unattended -nopause -nosplash -NullRHI -testexit="Automation Test Queue Empty"
```

Banc d'essai d'un scénario (le jeu se lance, joue le scénario puis quitte) :

```powershell
UnrealEditor.exe Impact.uproject -game -windowed -ImpactTest=<nom> -ImpactQuit=<secondes>
```

## Prérequis et dépendances

- Windows, **Unreal Engine 5.7**, Visual Studio avec la chaîne C++.
- Le plugin **[ALS Refactored](https://github.com/Sixze/ALS-Refactored)** (Sixze, licence MIT), **non inclus** dans ce dépôt. Le projet utilise une version locale légèrement modifiée (11 fichiers C++ et quelques assets) : l'installer tel quel ne suffit pas toujours pour compiler.
- Le contenu `Content/` du dépôt est limité à mes équipements et matériaux d'origine. Les animations Mixamo, le personnage et le pack d'armes utilisés pendant le développement **ne sont pas redistribués**.

Le dépôt montre donc le code et la démarche, mais **ne se lance pas tel quel** sans ces éléments.

## Limites

- Pas de mesure de performance (images par seconde, nombre de ragdolls simultanés) publiée.
- Pas de validation multijoueur ni de build empaqueté.
- Pas de démo vidéo dans le dépôt pour l'instant.

## Licence

Code sous licence MIT (voir `LICENSE`). Les composants tiers sont listés dans `THIRD_PARTY_NOTICES.md`. Unreal Engine est une marque d'Epic Games.
