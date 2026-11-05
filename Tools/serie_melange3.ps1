# Troisieme serie du 27 octobre : regarder (images temoins juste apres la balle) et relever l'entree au sol image par image.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
$Tout = "-ImpactElanGarde -ImpactChuteElan -ImpactChuteFinie -ImpactSolEnchaine"
foreach ($Config in @(
	@{ Nom = "m3_film_temoin"; Extra = "-ImpactFilmerBalle" },
	@{ Nom = "m3_film_elan"; Extra = "-ImpactFilmerBalle -ImpactElanGarde" },
	@{ Nom = "m3_film_fondu35"; Extra = "-ImpactFilmerBalle -ImpactElanGarde -ImpactFonduImpact=0.35" },
	@{ Nom = "m3_film_sanspose"; Extra = "-ImpactFilmerBalle -ImpactElanGarde -ImpactNoPose" })) {
	& $Banc -Nom $Config.Nom -Test fuite -Duree 14 -Runs 1 -Extra $Config.Extra | Select-Object -Last 1
}
& $Banc -Nom m3_entree_partie -Test partie -Duree 60 -Runs 1 -Extra "$Tout -ImpactSolEntreeImages" | Select-Object -Last 1
& $Banc -Nom m3_entree_soldats -Test soldats -Duree 42 -Runs 1 -Extra "$Tout -ImpactSolEntreeImages" | Select-Object -Last 1
"serie finie"
