# Équipements dans Impact

Ajout du 22 octobre 2026, directement dans le showcase ALS existant.

## Utilisation

- T ouvre la personnalisation, puis choisir **Personnage**. Fonctionne aussi sans arme.
- Choisir sans gilet, léger, moyen ou lourd ; activer ou retirer le casque.
- Sur le casque : aucune optique, nocturne ou thermique. La lampe est indépendante.
- N abaisse/remonte l'optique et active/désactive la vision choisie. H allume/éteint la lampe installée. L garde sa fonction de laser d'arme.
- Le bouton « Équiper le passant devant moi (essai) » copie la sélection au passant regardé avant d'ouvrir T. Refermer T pour lui tirer dessus. Le bouton est désactivé sans passant devant le joueur.
- Onglets **Test civils** et **Test armes** (ajout du 22 octobre au soir) : font apparaître des PNJ de test à 6 m devant le joueur, face à lui. Civils sans arme (ou 3 civils avec gilet moyen et casque) ; armés avec le choix de l'arme (fusil, pistolet à une ou deux mains), du gilet (aucun, léger, moyen, lourd) et du casque. « Retirer tous les PNJ de test » les enlève, avec les armes lâchées. Code : `UImpactGameplaySubsystem::SpawnTestPerson` / `RemoveTestPeople`. Banc caché : `-ImpactTest=pnjtest`.
- Le changement d'équipement est immédiat. Seule la charnière de l'optique est animée ; aucune animation d'habillage n'est annoncée.

## Protection effective

Le tir existant appelle `UImpactEquipmentComponent::DeliverBullet` avant l'anatomie, pour la première cible et chaque personnage traversé ensuite. Cette fonction interroge les zones de l'équipement, réduit la vitesse de la balle restante, puis appelle le système anatomique existant seulement si la balle passe. Une balle arrêtée ne crée pas de plaie perforante ni de sang. Une petite réaction physique et le comportement d'alerte existants restent déclenchés.

La sortie du corps est également contrôlée : une plaque arrière peut arrêter une balle qui vient de traverser le corps. La perte d'énergie à l'entrée n'est pas réappliquée à la même plaque à la sortie.

Les zones suivent la pose du torse/de la tête. Pas de colliders physiques supplémentaires qui poussent le personnage. Bras, jambes, cou et visage sont exposés. Le gilet lourd ajoute une protection basse des flancs ; les aisselles restent ouvertes. Les boîtes de requête simplifient les contours des plaques.

Les états avant/dos/flancs et casque s'usent séparément. Retirer puis remettre le même équipement ne le répare pas. Les trois modèles ont chacun leur état. Tout revient à neuf au prochain lancement du scénario ; pas de sauvegarde entre sessions.

Réglages de jeu modifiables dans `ImpactArmorRules.h` : résistance initiale 650 / 1850 / 3000 J pour les trois gilets, 850 J pour le casque, flancs lourds à 65 % de la résistance principale. Ce sont des valeurs choisies pour le jeu, **pas des classes certifiées ni une simulation fidèle de matériaux balistiques**. Pas de ricochet, fragmentation ou traumatisme interne derrière une plaque arrêtant le projectile. Le poids ne ralentit pas encore la locomotion.

## Sons d'impact sur la protection (ajout du 22 octobre au soir, interrupteur `-ImpactSonsArmure`)

Demande : « si les balles touchent le gilet, ça doit faire un bruit de métal, et si le gilet craque aussi ». Sons fabriqués par le code comme les autres sons du jeu (`ImpactAudio.cpp`), choisis dans `UImpactEquipmentComponent::ResolveContact` selon l'état réel de la plaque :

- chaque balle encaissée par une plaque : choc métallique qui résonne, plus fort selon l'énergie absorbée ; le casque sonne plus aigu et plus court que le gilet ;
- la plaque qui cède (la balle traverse, ou la plaque passe sous la moitié de sa résistance) : craquement sec en plus du métal ;
- la plaque hors d'usage (résistance à zéro) : rupture plus lourde, avec des débris qui tintent.

Le journal l'écrit sur chaque ligne `[protection] ... son=metal|metal+craque|metal+rupture`. Sans l'interrupteur, le son d'avant (balle sur un mur). `-ImpactSonsArmureWav` enregistre les quatre sons dans `Saved/_preuves_reprise/sons/` pour les écouter hors du jeu.

## Vision et lampe

La vision nocturne amplifie les détails déjà éclairés et comprime les hautes lumières ; elle ne voit pas dans un noir absolu sans lumière. La thermique représente les personnages visibles en couleur chaude et assombrit le décor. Elle ne mesure pas une température : ni refroidissement des cadavres, ni température des objets ne sont simulés.

Le test de profondeur empêche de révéler les personnages derrière les obstacles opaques. Les éléments translucides et le multijoueur ne sont pas validés. Le stencil 253 est réservé temporairement puis restauré en sortie du mode ; les usages existants incompatibles sont laissés intacts. `r.CustomDepth=3` est activé dans la configuration du projet.

La lampe projette réellement un éclairage depuis le casque, avec ombres. L'optique et la lampe suivent la tête. Les accessoires et gilets sont des meshes rigides : pas de tissu simulé ni de retouche de l'animation ALS.

## Intégration et limites

Les sources de physiologie, corps, réactions et ALS n'ont pas été modifiées par ce travail. Le joueur reçoit le composant d'équipement via son composant d'arme. Les passants peuvent le recevoir via le bouton d'essai. Le jeu actuel n'a pas encore de combat complet où des ennemis tirent sur le joueur, ni de physiologie du joueur : ce travail n'ajoute pas ces systèmes. La protection a été éprouvée sur des passants possédant déjà l'anatomie.

**Pour les futurs tirs ennemis et les outils de test**, passer par `DeliverBullet`. Un appel direct à `UImpactBodyComponent::ReceiveBullet` reste une entrée anatomique qui contourne l'équipement. Les anciens scénarios anatomiques qui l'appellent directement ne sont pas réécrits ici.

Modèles originaux créés dans Blender : trois gilets, casque, module nocturne, module thermique, lampe. Sources et FBX conservés dans ce dossier. Les détails de géométrie/import sont dans `Saved/Equipment/models_notes.md` ; les détails du filtre dans `Saved/Equipment/vision_notes.md`.

Les fichiers existants modifiés ont été sauvegardés dans `Saved/Equipment/before`. Contrôle reproductible : `Tools/Equipment/verify_equipment.ps1 -Label nouveau_nom`, qui lance et ferme uniquement son propre processus Unreal.
