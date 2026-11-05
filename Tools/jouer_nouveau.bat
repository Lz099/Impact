@echo off
rem Comme jouer_sol.bat, avec ce qui a ete fait les 21 et 22 octobre, pour le regarder en jeu :
rem   - au sol, l'animation est jouee pour de vrai (-ImpactSolAnime) : le ragdoll fait la chute et
rem     l'amene jusqu'a la pose, puis l'animation prend le relais ; une balle, une bousculade, un
rem     evanouissement ou la mort le refont ragdoll, et il reprend l'animation ensuite
rem   - le trajet vers la pose sans moulinet ni redressement
rem     (-ImpactSolCible -ImpactSolSansMuscles -ImpactSolMainsLibres -ImpactSolAmortiActif -ImpactSolSansFlail -ImpactSolAncre)
rem   - la posture qui revient apres une deuxieme chute, meme confus (-ImpactSolReArme)
rem   - le sang qui marque la peau autour des plaies (-ImpactSangPeau)
rem   - les passants armes : fusil, pistolet a une main, pistolet a deux mains (-ImpactPNJArmes) ;
rem     ils visent le joueur (haut du corps anime pendant qu'ils visent), gardent l'arme d'une main
rem     et tiennent la plaie de l'autre, et ne la lachent que tres affaiblis
rem   - la chute : plus de moulinet ni de salto, les genoux lachent et il s'effondre (-ImpactChuteTenue)
rem   - un corps au sol bouscule part d un bloc et ne sert plus de marche (-ImpactBourradeBloc)
rem   - la chute jouee en animation (-ImpactChuteAnimee) : il tombe pour de vrai, puis se pose au sol
rem   - l'arme reste dans la main pendant la chute et au sol, au lieu de flotter en l'air (-ImpactArmeEnMain)
rem   - les morts : plus de collision avec les vivants ni la camera, armes lachees comprises (-ImpactMortTraversable)
rem   - le fusil lache des qu'il est blesse ou qu'il tombe ; le pistolet reste en main (-ImpactFusilLache)
rem   - le retournement pour ramper, doux, et la fin de chute sans coup de fouet (-ImpactRetournementDoux -ImpactFinChuteDouce)
rem   - la chute reste sur sa derniere image : plus de retour debout s'il y a un a-coup (-ImpactChuteFigee)
rem   - si le jeu se fige 8 s, on ecrit ou il est bloque dans Saved\_preuves_reprise\gel_*.txt (-ImpactGuetteGel)
rem   - balle sur un gilet ou un casque : bruit de metal ; plaque qui cede : craquement ; plaque hors d'usage : rupture (-ImpactSonsArmure)
rem   - blesse au ventre, il se courbe en avant (-ImpactVentreCourbe)
rem   - animation calculee sur le fil principal : contre le plantage "index ... into an array of size 0" des muscles (-ImpactAnimSequentielle)
rem 23 octobre, animations Mixamo (Tools/Mixamo_Chutes/recu_23_10, importees par Tools/import_mixamo_23_10.py) :
rem   - 7 chutes et morts de plus, prises toutes seules par la chute animee (Chute_Avant_3 a 6, Chute_Arriere_4 et 5, Chute_Cote_3)
rem   - au sol, les prises Mixamo selon la blessure et le cote sur lequel il est couche : sur le dos il se tord, gemit ou
rem     halete, tres affaibli il agonise ; sur le ventre il se traine sur les bras, et rampe pour de vrai s'il veut s'eloigner
rem     (-ImpactSolMixamo, plus de retournement par la physique)
rem   - le relever Mixamo, du dos (blesse s'il l'est) ou du ventre, sans teleportation a la fin (-ImpactReleveMixamo)
rem   - les PNJ armes te tirent dessus des le premier coup de feu ; vie en bas a gauche (-ImpactPNJTirent)
rem   - la chute melangee (-ImpactChuteMelee, remplace la chute jouee seule) : il devient physique des la balle, la poussee et
rem     la gravite le font tomber, et l'animation de chute (choisie selon le sens de la poussee) sert de muscles qui faiblissent
rem     a mesure qu'il tombe
rem   - les soldats (-ImpactPNJCouvert) : un PNJ arme cherche un abri (derriere une caisse, au coin d'un mur), y court,
rem     s'accroupit, se leve ou sort de cote pour tirer une rafale et se recache ; il change d'abri quand tu bouges autour de
rem     lui, que tu le vois, que tu t'approches, ou te contourne s'il ne te voit plus ; il ne tire pas a travers un camarade
rem   - a terre avec son arme (-ImpactSolArme) : il la garde (sauf bras touche), couche sur le ventre il te vise et tire ; vu
rem     trop longtemps, il rampe vers un abri proche
rem 26-27 octobre, les soldats (rien a allumer, c'est dans -ImpactPNJCouvert / -ImpactPNJTirent deja la) :
rem   - ils voyaient 22 accessoires et aucun des 172 murs et piliers de la carte, qui sont des Blueprints :
rem     ils se battaient donc a decouvert. Temps passe sans abri mesure au banc : 58 % avant, 35 % apres
rem   - ils te touchent et te tuent : 0 touche sur 23 tirs a 22,9 m avant, 33-36 % de touches a 9 m apres
rem   - la rafale existe (le temps de reaction etait retire au hasard entre deux balles de la meme rafale)
rem   - tous au fusil : trois sur cinq se promenaient avec un pistolet, et ALS n'a de pose de course qu'au fusil
rem   - a couvert, la tete balaye au lieu de rester braquee sur toi a travers le mur (19 deg de moyenne)
rem   - un blesse grave lache son fusil au lieu de le garder soude dans la paume, mort compris
rem   Trois reglages a balayer si besoin : -ImpactSoldatPortee=1500 (distance de tir, cm),
rem   -ImpactSoldatRepli=2 (attente avant de se repositionner, s), -ImpactSoldatMarche=0.35 (cout d'un
rem   metre de marche vers un abri : plus bas = il traverse plus volontiers le terrain nu).
rem 27 octobre, le melange animations / ragdoll (Saved\_preuves_reprise\MELANGE_ANIM_RAGDOLL.md) :
rem   - touche en pleine course, il garde sa vitesse au lieu de s'arreter net (-ImpactElanGarde) : 76 %% de sa
rem     vitesse la seconde suivante contre 30 %% avant (banc fuite, 5+5 essais) ; un soldat qui court n'est plus
rem     force a marcher en se tournant vers le tireur
rem   - touche en pleine course, plus de prise d'impact jouee debout par-dessus la course (-ImpactCourseSansPose) :
rem     c'etait la "pose zero" ; il reagit avec son corps (impulsion, muscles, main sur la plaie)
rem   - la chute va jusqu'au bout avant la posture au sol (-ImpactChuteFinie) : 0 chute coupee sur 26 contre 18 sur 26
rem     (mesure, pas encore regarde a l'oeil)
rem 29 octobre, l'attitude de gang (-ImpactAttitudeGang), posee par-dessus les animations d'ALS (jambes, chutes, sol :
rem   rien ne change, et elle s'efface des qu'il tombe, se releve ou se tient une plaie) :
rem   - il vise le pistolet couche sur le cote, le fusil penche et plus bas, le torse bombe, la tete penchee vers l'arme,
rem     un recul exagere a chaque balle ; le canon reste sur toi (0,1 a 0,8 deg d'ecart moyen mesure au banc)
rem   - en courant au pistolet, l'arme levee a cote de la tete ; en marchant, il roule des epaules
rem   - il te pointe du doigt quand il te repere ou qu'un pote tombe, provoque entre deux rafales (bras ecartes,
rem     se frappe la poitrine, "viens"), fete l'arme en l'air quand il te touche ou que tu tombes
rem   - avant d'etre alerte, le gang traine sur son coin et parle avec les mains ; au combat il tire debout ;
rem     un sur deux a un pistolet
rem   Les gestes sont calcules ; des animations Mixamo (Tools/import_gang.py) les remplacent quand elles existent.
rem 29 octobre, un corps lourd ("un corps c'est lourd, c'est pas ragdoll moon low gravity") :
rem   - entre deux balles le haut du corps est anime, plus physique en permanence (-ImpactCorpsAnime) : en course, les bras
rem     trainaient de 36 deg en moyenne derriere l'animation (jusqu'a 174, dans le dos), la tete de 24 ; maintenant 0 a 3 deg.
rem     La balle rend toujours physique la partie touchee, le temps de la reaction
rem   - le bras qui va tenir une plaie est amorti, il ne fouette plus en courant (-ImpactBrasAmorti=0.5)
rem   - la chute guidee ne retient plus le corps vers le haut et joue son animation plus vite (-ImpactChuteLourde) :
rem     au sol en 0,36 a 0,76 s au lieu de 1,1 a 2,1 s ; plus de petit saut au debut de la chute (0 a 0,7 cm au lieu de 11,6)
rem 30 octobre :
rem   - l'arme d'un mort tombe par terre et y reste (rien a ajouter : dans DropWeapon). Avant, ALS l'avait deja effacee dans le
rem     ragdoll et rien n'etait lache : elle s'evanouissait. Mesure : chaque mort lache maintenant une arme, posee a 2-7 cm du sol
rem   - l'arme reste dessinee dans la main pendant toute la chute (-ImpactArmeVisible), au lieu de disparaitre jusqu'au sol
rem   - les gestes de gang Mixamo (provocations, cris, fetes, attente, conversations), importes par Tools/import_gang.py
rem Tout est eteint par defaut : jouer.bat et jouer_sol.bat restent tels quels pour comparer.
start "" "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "D:\UEProjets\Impact\Impact.uproject" /ALS/ALSExtras/Levels/L_Als_Playground -game -windowed -resx=1600 -resy=900 -nosplash -ImpactSolRagdoll -ImpactSolCible -ImpactSolSansMuscles -ImpactSolMainsLibres -ImpactSolAmortiActif -ImpactSolSansFlail -ImpactSolAncre -ImpactSolReArme -ImpactSolAnime -ImpactSangPeau -ImpactPNJArmes -ImpactChuteTenue -ImpactBourradeBloc -ImpactChuteAnimee -ImpactPoseImmediate -ImpactResteAuSol -ImpactArmeEnMain -ImpactMortTraversable -ImpactFusilLache -ImpactRetournementDoux -ImpactFinChuteDouce -ImpactChuteFigee -ImpactGuetteGel -ImpactSonsArmure -ImpactAnimSequentielle -ImpactVentreCourbe -ImpactSolMixamo -ImpactReleveMixamo -ImpactPNJTirent -ImpactChuteMelee -ImpactPNJCouvert -ImpactSolArme -ImpactElanGarde -ImpactCourseSansPose -ImpactChuteFinie -ImpactAttitudeGang -ImpactCorpsAnime -ImpactBrasAmorti=0.5 -ImpactChuteLourde -ImpactArmeVisible
