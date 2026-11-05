# Trois animations au sol — première version

Créées dans Blender sur le mannequin ALS fourni. Il s'agit de mouvements construits
par poses et résolution des bras/jambes, pas de capture de mouvement.

| Animation | Intention | Limites |
| --- | --- | --- |
| Recroqueville_Au_Sol | Sur le côté droit, protéger le ventre à deux mains, se contracter puis relâcher | Commence déjà couché ; pas de transition depuis une chute |
| Halete_Au_Sol | Sur le dos, respirer avec effort, une main sur le thorax | Pas d'animation faciale ni de son ; amplitude à coordonner avec l'état physiologique |
| Tient_Jambe_Au_Sol | Assis, appui sur la main gauche, protection de la cuisse droite, petit ajustement puis crispation | Version jambe droite uniquement ; ne convient pas automatiquement à une lésion gauche ou à un bras gauche inutilisable |

Chaque boucle dure 5 secondes : 151 échantillons, de 0 à 150, à 30 images/s.
La dernière image répète la première. La racine ne se déplace pas.
Les positions du bassin diffèrent selon la posture ; il n'est pas bloqué à l'origine.
Le squelette, la hiérarchie et la référence d'origine sont conservés.

Dans chaque dossier :

- `.blend` : scène source modifiable, mannequin, caméras et sol d'aperçu ;
- `.fbx` : squelette et animation, sans maillage, export binaire en centimètres ;
- `Apercu.gif` : deux boucles à vitesse normale, puis une au ralenti, profil et trois quarts ;
- `Positions.png` : quelques poses clés ;
- `export_validation.json` : comparaison de tous les échantillons après réimportation dans Blender.

## État dans Unreal

Les trois séquences sont importées dans `/Game/Impact/Anims/Sol_Creation_v01`,
sur `/ALS/ALS/Character/SK_Als`. Leur durée, les pistes réellement animées et le raccord
de boucle sont vérifiés par `unreal_import.json`.

Elles ne remplacent pas encore `Douleur_Au_Sol` en jeu. Le code actuel cherche les trois noms
directement dans `/Game/Impact/Anims`, et non dans ce sous-dossier. C'est volontaire :
ces boucles seules ne résolvent ni le passage de la pose de chute à la pose voulue,
ni le mélange avec les moteurs physiques et les collisions.

En particulier, une pose assise ne doit pas tirer instantanément un corps couché vers elle.
L'entrée et la sortie doivent dépendre des appuis, des bras disponibles, de la conscience
et des membres blessés. Le mouvement ne doit pas déplacer le corps par glissement.
Ne pas jouer ces séquences pendant la locomotion debout.

## Vérifications et limites

- Les rendus de poses ont été examinés et corrigés pour les mains et les appuis.
- Les exports ont été réimportés dans Blender et échantillonnés sur toutes les images :
  poses et maillage comparés à la scène source, référence et hiérarchie contrôlées.
- La présence du mouvement a été vérifiée après importation dans Unreal.
- Cela ne constitue pas une validation visuelle dans le jeu avec Chaos.
- Pas de validation médicale : ce sont trois intentions de jeu, pas des réactions universelles.
- Les contacts concernent un sol plat ; aucune adaptation au décor n'est contenue dans le FBX.
- Les vérifications automatiques du sol ne prouvent pas l'absence de toute auto-intersection
  entre les différentes parties du personnage.

## Reproduction

Scripts dans `Tools/AnimationsSol` :

1. `author_ground.py` construit les poses et les boucles dans Blender.
2. `inspect_contacts.py` contrôle les distances au sol.
3. `export_validate.py` exporte et réimporte les fichiers ; cet utilitaire reprend
   le pipeline de validation déjà présent dans les travaux précédents du projet.
4. `render_loops.py` et `package_previews.py` fabriquent les aperçus.
5. `import_unreal.py` importe uniquement dans le dossier d'évaluation ci-dessus.

Le dossier `Saved/AnimationsSol` contient les rendus intermédiaires et les journaux.
Cette copie dans `ArtSource` conserve les livrables en dehors des fichiers temporaires d'Unreal.
